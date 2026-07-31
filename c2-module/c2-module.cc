/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the
 * disclaimer below) provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *     * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 * GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 * HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "c2-module.h"

#include <dlfcn.h>
#include <sstream>
#include <map>

#include <C2PlatformSupport.h>
#include <C2ParamInternal.h>
#if !defined(ANDROID)
#include <C2AllocatorGBM.h>
#endif // !ANDROID

#define MAX_CIRCLE_POOL_BUFS      (16)

#define ALIGN(num, to) (((num) + (to - 1)) & (~(to - 1)))

std::shared_ptr<QC2ComponentStoreFactory> C2Factory::factory_video_ = nullptr;
std::shared_ptr<QC2ComponentStoreFactory> C2Factory::factory_audio_ = nullptr;
std::mutex C2Factory::lock_;

template<typename ...Args> std::runtime_error Exception(Args&&... args) {

  std::stringstream s;
  ((s << std::forward<Args>(args)), ...);
  return std::runtime_error(s.str());
}

namespace {

struct C2StructLayout {
  uint32_t size;
  uint32_t alignment;
};

bool AddWithoutOverflow(uint64_t left, uint64_t right, uint32_t* result) {
  if ((left > std::numeric_limits<uint32_t>::max()) ||
      (right > std::numeric_limits<uint32_t>::max() - left))
    return false;

  *result = static_cast<uint32_t>(left + right);
  return true;
}

bool GetStructLayout(const std::shared_ptr<C2ParamReflector>& reflector,
    const C2StructDescriptor& desc, C2StructLayout* layout) {

  // C2 structures are declared with C2_PACK, whose minimum alignment is 4.
  uint32_t alignment = alignof(uint32_t);
  uint64_t size = 0;

  for (const C2FieldDescriptor& field : desc) {
    uint32_t field_size = _C2ParamInspector::GetSize(field);
    uint32_t field_alignment = 1;

    if (field.type() & C2FieldDescriptor::STRUCT_FLAG) {
      std::unique_ptr<C2StructDescriptor> subdesc = reflector->describe(
          C2Param::CoreIndex(field.type()).coreIndex());
      if (subdesc == nullptr)
        return false;

      C2StructLayout sublayout;
      if (!GetStructLayout(reflector, *subdesc, &sublayout))
        return false;

      field_alignment = sublayout.alignment;
    } else {
      switch (field.type()) {
        case C2FieldDescriptor::INT64:
        case C2FieldDescriptor::UINT64:
        case C2FieldDescriptor::CNTR64:
          field_alignment = alignof(uint64_t);
          break;
        case C2FieldDescriptor::INT32:
        case C2FieldDescriptor::UINT32:
        case C2FieldDescriptor::CNTR32:
        case C2FieldDescriptor::FLOAT:
          field_alignment = alignof(uint32_t);
          break;
        case C2FieldDescriptor::STRING:
        case C2FieldDescriptor::BLOB:
          field_alignment = 1;
          break;
        default:
          return false;
      }
    }

    alignment = std::max(alignment, field_alignment);

    uint64_t field_end = _C2ParamInspector::GetOffset(field);
    if (field.extent() != 0)
      field_end += static_cast<uint64_t>(field_size) * field.extent();

    size = std::max(size, field_end);
  }

  size = (size + alignment - 1) & ~(static_cast<uint64_t>(alignment) - 1);
  if (size > std::numeric_limits<uint32_t>::max())
    return false;

  layout->size = static_cast<uint32_t>(size);
  layout->alignment = alignment;
  return true;
}

}  // namespace

C2ParamFieldMap::C2ParamFieldMap(std::shared_ptr<C2ParamReflector> reflector)
    : reflector_(reflector) {
}

void C2ParamFieldMap::AddStructDesc(C2Param::Index index,
    const std::string& path, uint32_t offset, uint32_t base_size,
    const C2StructDescriptor& desc) {

  for (const C2FieldDescriptor& field : desc) {
    std::string name = path + "." + field.name();
    uint32_t fieldoffset = 0;
    if (!AddWithoutOverflow(offset, _C2ParamInspector::GetOffset(field),
            &fieldoffset)) {
      continue;
    }

    // Recurse into nested structures, provided they can be described.
    if (field.type() & C2FieldDescriptor::STRUCT_FLAG) {
      if (field.extent() != 1) {
        continue;
      }

      std::unique_ptr<C2StructDescriptor> subdesc = reflector_->describe (
          C2Param::CoreIndex(field.type()).coreIndex());
      if (subdesc == nullptr) {
        continue;
      }

      AddStructDesc (index, name, fieldoffset, base_size, *subdesc);
      continue;
    }

    // Numeric fields, either single valued, a fixed size array or a flexible
    // array. The latter is reported with an extent of 0 and, as the Codec2
    // structure definitions place it last, extends to the end of the parameter.
    switch (field.type()) {
      case C2FieldDescriptor::INT32:
      case C2FieldDescriptor::UINT32:
      case C2FieldDescriptor::CNTR32:
      case C2FieldDescriptor::INT64:
      case C2FieldDescriptor::UINT64:
      case C2FieldDescriptor::CNTR64:
      case C2FieldDescriptor::FLOAT:
      case C2FieldDescriptor::STRING:
      case C2FieldDescriptor::BLOB:
        break;
      default:
        continue;
    }

    // The offset of a param field is relative to the start of the parameter,
    // which includes the parameter header. The size is that of a single
    // element, so that a supported values query refers to the first one.
    uint32_t param_offset = 0;
    if (!AddWithoutOverflow(sizeof(C2Param), fieldoffset, &param_offset))
      continue;

    _C2FieldId fieldid(param_offset, _C2ParamInspector::GetSize(field));

    fields_.push_back ({
        name,
        _C2ParamInspector::CreateParamField (index, fieldid),
        field.type(),
        static_cast<uint32_t>(field.extent()),
        base_size,
    });
  }
}

uint32_t C2ParamFieldMap::AddParamDescriptors(
    const std::vector<std::shared_ptr<C2ParamDescriptor>>& descriptors) {

  uint32_t n_params = 0;

  for (const std::shared_ptr<C2ParamDescriptor>& descriptor : descriptors) {
    if (descriptor == nullptr)
      continue;

    if (descriptor->isReadOnly())
      continue;

    // Requested parameters are aliases of their actual counterpart.
    if (descriptor->index() & C2Param::CoreIndex::IS_REQUEST_FLAG)
      continue;

    C2Param::Index index = descriptor->index();

    std::unique_ptr<C2StructDescriptor> desc =
        reflector_->describe (index.coreIndex());
    if (desc == nullptr) {
      continue;
    }

    uint32_t base_size = sizeof(C2Param);
    if (index & C2Param::CoreIndex::IS_FLEX_FLAG) {
      C2StructLayout layout;
      if (!GetStructLayout(reflector_, *desc, &layout) ||
          !AddWithoutOverflow(sizeof(C2Param), layout.size, &base_size)) {
        continue;
      }
    }

    std::string name = descriptor->name();

    // Prefix vendor parameters to match the naming used by the Codec2 clients.
    if (index.isVendor())
      name = "vendor." + name;

    AddStructDesc (index, name, 0 /* offset */, base_size, *desc);
    n_params++;
  }

  return n_params;
}

bool C2ParamFieldMap::FindField(const std::string& name,
    C2Param::Index* index, uint32_t* offset, uint32_t* size,
    C2FieldDescriptor::type_t* type, uint32_t* extent) const {

  return FindField(name, index, offset, size, type, extent, nullptr);
}

bool C2ParamFieldMap::FindField(const std::string& name,
    C2Param::Index* index, uint32_t* offset, uint32_t* size,
    C2FieldDescriptor::type_t* type, uint32_t* extent,
    uint32_t* base_size) const {

  for (const FieldInfo& info : fields_) {
    if (info.name != name)
      continue;

    *index = _C2ParamInspector::GetIndex (info.field);
    *offset = _C2ParamInspector::GetOffset (info.field);
    *size = _C2ParamInspector::GetSize (info.field);
    *type = info.type;

    if (extent != nullptr)
      *extent = info.extent;

    if (base_size != nullptr)
      *base_size = info.base_size;

    return true;
  }

  return false;
}

std::unique_ptr<C2Param> C2ParamFieldMap::AllocParam(C2Param::Index index,
    uint32_t size, const C2Param* source) {

  if (size < sizeof (C2Param)) {
    return nullptr;
  }

  // Codec2 has no allocator for a parameter of a size only known at runtime,
  // as the typed ones need the parameter type at compile time. Build the
  // parameter the same way the Codec2 marshalling does, by laying out the
  // header in front of the payload and letting C2Param::From() validate it.
  // The allocation mirrors C2Param::Copy(), which is what the component uses
  // for the heap parameters it returns from a query: plain operator new,
  // released through the default deleter of std::unique_ptr<C2Param> as that
  // is the type the Codec2 configuration API takes.
  void *memory = ::operator new (size, std::nothrow);
  if (memory == nullptr) {
    return nullptr;
  }

  memset (memory, 0, size);

  // C2Param is a standard layout type beginning with the size, followed by the
  // index, both 32 bit. Composing the header by hand is what allows a size
  // which does not match any of the compile time known parameter sizes.
  const uint32_t header[2] = { size, static_cast<uint32_t>(index) };
  memcpy (memory, header, sizeof (header));

  if ((source != nullptr) && (source->size () > sizeof (C2Param))) {
    uint32_t payload = std::min (
        static_cast<uint32_t>(source->size ()), size) - sizeof (C2Param);

    memcpy (reinterpret_cast<uint8_t*>(memory) + sizeof (C2Param),
        reinterpret_cast<const uint8_t*>(source) + sizeof (C2Param), payload);
  }

  C2Param *param = C2Param::From (memory, size);
  if (param == nullptr) {
    ::operator delete (memory);
    return nullptr;
  }

  return std::unique_ptr<C2Param> (param);
}

std::shared_ptr<C2GraphicBlock> C2GraphicMemory::Fetch(uint32_t width,
                                                       uint32_t height,
                                                       C2PixelFormat format,
                                                       bool isheic) {

  if (width == 0 || height == 0) {
    throw Exception("One or more dimensions are 0 !");
  }

  uint32_t fmt = 0;
  C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
  std::shared_ptr<C2GraphicBlock> block;

#if !defined(ANDROID)
  switch (format) {
    case C2PixelFormat::kNV12:
      fmt = isheic ? GBM_FORMAT_IMPLEMENTATION_DEFINED : GBM_FORMAT_NV12;
      if (isheic) {
#ifdef GBM_BO_USAGE_PRIVATE_HEIF
        usage.expected |= GBM_BO_USAGE_PRIVATE_HEIF;
#else
        throw Exception("HEIF is not supported in GBM!");
#endif // GBM_BO_USAGE_PRIVATE_HEIF
      }
      break;
    case C2PixelFormat::kNV12UBWC:
      fmt = GBM_FORMAT_NV12;
      usage.expected |= GBM_BO_USAGE_UBWC_ALIGNED_QTI;
      break;
    case C2PixelFormat::kP010:
      fmt = GBM_FORMAT_YCbCr_420_P010_VENUS;
      break;
    case C2PixelFormat::kTP10UBWC:
      fmt = GBM_FORMAT_YCbCr_420_TP10_UBWC;
      usage.expected |= GBM_BO_USAGE_UBWC_ALIGNED_QTI;
      break;
    default:
      throw Exception("Failed to create C2Buffer! Unsupported format!");
  }
#else // !ANDROID
    fmt = static_cast<uint32_t>(format);
#endif // ANDROID

  auto status = pool_->fetchGraphicBlock(width, height, fmt, usage, &block);
  if (status != C2_OK) {
    throw Exception("Unable to create graphic block, error: ", status, " !");
  }

  return block;
}

std::shared_ptr<C2LinearBlock> C2LinearMemory::Fetch(uint32_t size) {

  if (size == 0) {
    throw Exception("Size is 0 !");
  }

  uint32_t capacity = ALIGN(size, 4096);
  C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
  std::shared_ptr<C2LinearBlock> block;

  auto status = pool_->fetchLinearBlock(capacity, usage, &block);
  if (status != C2_OK) {
    throw Exception("Unable to create linear block, error: ", status, " !");
  }

  return block;
}

C2Module::C2Module (std::shared_ptr<C2Component>& component,
                     std::shared_ptr<C2ComponentStore>& store, C2ModeType mode)
    : component_(component),
      store_(store),
      state_(State::kCreated),
      mode_(mode),
      notifier_(nullptr) {

  // Get local pointer to the underlying component interface.
  interface_ = std::shared_ptr<C2ComponentInterface>(component_->intf());
}

C2Module::~C2Module () { }

c2_status_t C2Module::Initialize(std::shared_ptr<IC2Notifier>& notifier,
    C2Module::PoolType pool_type) {

  std::lock_guard<std::mutex> lk(lock_);
  auto listener = std::make_shared<C2Listener> (this);

  auto status = component_->setListener_vb (listener, C2_MAY_BLOCK);
  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to set events listener, error ", status, "!");
  }

  if (!notifier) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Invalid notifier argument!");
  }

  notifier_ = notifier;
  state_ = State::kIdle;

#if (CODEC2_CONFIG_VERSION_MAJOR == 2)
  C2Allocator::id_t pool_id;
  bool is_graphic_pool = false;

  // Default pool type option
  switch (mode_) {
    case C2ModeType::kVideoEncode:
      pool_id = C2AllocatorStore::LINEAR_NON_CONTIGUOUS;
      is_graphic_pool = false;
      break;
    case C2ModeType::kVideoDecode:
      pool_id = C2AllocatorStore::GRAPHIC_NON_CONTIGUOUS;
      is_graphic_pool = true;
      break;
    case C2ModeType::kAudioEncode:
    case C2ModeType::kAudioDecode:
      // Output buffer pool for audio is not properly supported.
    default:
      return C2_OK;
  }
  std::shared_ptr<C2BlockPool> pool;

  // Update pool type via input
  switch (pool_type) {
    case C2Module::PoolType::kUnspecified:
      break;
    case C2Module::PoolType::kDefaultLinear:
      pool_id = C2AllocatorStore::DEFAULT_LINEAR;
      is_graphic_pool = false;
      break;
    case C2Module::PoolType::kDefaultGraphic:
      pool_id = C2AllocatorStore::DEFAULT_GRAPHIC;
      is_graphic_pool = true;
      break;
    case C2Module::PoolType::kLinearContiguous:
      pool_id = C2AllocatorStore::LINEAR_CONTIGUOUS;
      is_graphic_pool = false;
      break;
    case C2Module::PoolType::kLinearNonContiguous:
      pool_id = C2AllocatorStore::LINEAR_NON_CONTIGUOUS;
      is_graphic_pool = false;
      break;
    case C2Module::PoolType::kGraphicContiguous:
      pool_id = C2AllocatorStore::GRAPHIC_CONTIGUOUS;
      is_graphic_pool = true;
      break;
    case C2Module::PoolType::kGraphicNonContiguous:
      pool_id = C2AllocatorStore::GRAPHIC_NON_CONTIGUOUS;
      is_graphic_pool = true;
      break;
    default:
      break;
  }

  status = ::android::CreateCodec2BlockPool(pool_id, component_, &pool);

  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Unable to create output block pool, error: ", status, "!");
  }

  C2BlockPool::local_id_t id;
  if (is_graphic_pool) {
    graphic_mem_ = std::make_shared<C2GraphicMemory>(pool);
    id = graphic_mem_->GetLocalId();
  } else {
    linear_mem_ = std::make_shared<C2LinearMemory>(pool);
    id = linear_mem_->GetLocalId();
  }

  // Register the buffer pool ID so that it is used by the component.
  auto pools = C2PortBlockPoolsTuning::output::AllocUnique({id});
  auto param = C2Param::Copy(*pools);

  std::vector<std::unique_ptr<C2SettingResult>> failures;
  status = interface_->config_vb ({param.get()}, C2_MAY_BLOCK, &failures);

  if ((status != C2_OK) || !failures.empty()) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to set parameter, error ", status, "!");
  }
#endif // CODEC2_CONFIG_VERSION_MAJOR

  return C2_OK;
}

std::shared_ptr<C2GraphicMemory> C2Module::GetGraphicMemory() {

  std::lock_guard<std::mutex> lk(lock_);

  if (!graphic_mem_) {
    std::shared_ptr<C2BlockPool> pool;

    auto status = ::android::GetCodec2BlockPool(
        C2AllocatorStore::DEFAULT_GRAPHIC, component_, &pool);

    if (status != C2_OK) {
      throw Exception("Component[", interface_->getName().c_str(), "]: "
          "Unable to get graphic block pool, error: ", status, "!");
    }

    graphic_mem_ = std::make_shared<C2GraphicMemory>(pool);
  }

  return graphic_mem_;
}

std::shared_ptr<C2LinearMemory> C2Module::GetLinearMemory() {

  std::lock_guard<std::mutex> lk(lock_);

  if (!linear_mem_) {
    std::shared_ptr<C2BlockPool> pool;

    auto status = ::android::GetCodec2BlockPool(
        C2AllocatorStore::DEFAULT_LINEAR, component_, &pool);

    if (status != C2_OK) {
      throw Exception("Component[", interface_->getName().c_str(), "]: "
          "Unable to get linear block pool, error: ", status, "!");
    }

    linear_mem_ = std::make_shared<C2LinearMemory>(pool);
  }

  return linear_mem_;
}

#if defined(ENABLE_AUDIO_PLUGINS)
std::shared_ptr<qc2audio::QC2BufferCirclePools>
C2Module::GetLinearCirclePool(uint32_t size) {

  std::lock_guard<std::mutex> lk(lock_);

  if (!linear_circle_pool_) {
    std::shared_ptr<C2BlockPool> pool;

    auto status = ::android::GetCodec2BlockPool(
        C2AllocatorStore::DEFAULT_LINEAR, component_, &pool);

    if (status != C2_OK) {
      throw Exception("Component[", interface_->getName().c_str(), "]: "
          "Unable to get linear block pool, error: ", status, "!");
    }

    std::shared_ptr<qc2audio::QC2LinearBufferPool> linear_pool =
        std::make_unique<qc2audio::QC2LinearBufferPool>(pool,
        C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE);
    linear_pool->setBufferSize(size);

    linear_circle_pool_ =
        std::make_shared<qc2audio::QC2BufferCirclePools>(
        MAX_CIRCLE_POOL_BUFS, linear_pool);
  }

  return linear_circle_pool_;
}
#endif //ENABLE_AUDIO_PLUGINS

std::unique_ptr<C2Param> C2Module::QueryParam(C2Param::Index index) {

  std::vector<std::unique_ptr<C2Param>> params;
  auto status = interface_->query_vb({}, {index}, C2_MAY_BLOCK, &params);

  if ((status != C2_OK) || params.empty()) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to query parameter, error ", status, "!");
  }

  return std::move(params.at(0));
}

c2_status_t C2Module::SetParam(std::unique_ptr<C2Param>& param) {

  std::vector<std::unique_ptr<C2SettingResult>> failures;
  auto status = interface_->config_vb ({param.get()}, C2_MAY_BLOCK, &failures);

  if ((status != C2_OK) || !failures.empty()) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to set parameter, error ", status, "!");
  }

  return C2_OK;
}

template<typename T>
static bool WriteNativeInt(int64_t value, void* field) {

  if ((value < static_cast<int64_t>(std::numeric_limits<T>::min())) ||
      (value > static_cast<int64_t>(std::numeric_limits<T>::max())))
    return false;

  T native = static_cast<T>(value);
  memcpy(field, &native, sizeof(native));
  return true;
}

static bool WriteNativeValue(int64_t integer, double number, bool is_float,
    C2FieldDescriptor::type_t type, uint32_t size, void* field) {

  if (is_float) {
    if (size != sizeof(float))
      return false;

    float native = static_cast<float>(number);
    memcpy(field, &native, sizeof(native));
    return true;
  }

  switch (type) {
    case C2FieldDescriptor::INT32:
      return (size == sizeof(int32_t)) && WriteNativeInt<int32_t>(integer, field);
    case C2FieldDescriptor::UINT32:
    case C2FieldDescriptor::CNTR32:
      return (size == sizeof(uint32_t)) && WriteNativeInt<uint32_t>(integer, field);
    case C2FieldDescriptor::INT64:
      return (size == sizeof(int64_t)) && WriteNativeInt<int64_t>(integer, field);
    case C2FieldDescriptor::UINT64:
    case C2FieldDescriptor::CNTR64:
      return (size == sizeof(uint64_t)) && WriteNativeInt<uint64_t>(integer, field);
    default:
      return false;
  }
}

static bool AnyToScalar(const std::any& value, int64_t* integer) {

  if (auto v = std::any_cast<int32_t>(&value))  { *integer = *v; return true; }
  if (auto v = std::any_cast<uint32_t>(&value)) { *integer = *v; return true; }
  if (auto v = std::any_cast<int64_t>(&value))  { *integer = *v; return true; }
  if (auto v = std::any_cast<uint64_t>(&value)) { *integer = static_cast<int64_t>(*v); return true; }

  return false;
}

// Extracts a float from `value`, the native C2 FLOAT field type.
static bool AnyToScalar(const std::any& value, double* number) {

  if (auto v = std::any_cast<float>(&value)) { *number = *v; return true; }

  return false;
}

// Translates `value` into the native byte representation of `type`.
static bool AnyToBuffer(const std::any& value, C2FieldDescriptor::type_t type,
    uint32_t size, std::vector<uint8_t>* buffer) {

  if (type == C2FieldDescriptor::STRING) {
    if (size != 1)
      return false;

    if (auto v = std::any_cast<std::string>(&value)) {
      buffer->assign(v->begin(), v->end());
    } else {
      return false;
    }

    if (buffer->empty() || buffer->back() != 0)
      buffer->push_back(0);

    return true;
  }

  if (type == C2FieldDescriptor::BLOB) {
    if (size != 1)
      return false;

    if (auto v = std::any_cast<std::vector<uint8_t>>(&value)) {
      *buffer = *v;
      return true;
    }

    return false;
  }

  bool is_float = (type == C2FieldDescriptor::FLOAT);

  int64_t integer = 0;
  double number = 0.0;

  bool ok = is_float ? AnyToScalar(value, &number) : AnyToScalar(value, &integer);
  if (!ok)
    return false;

  buffer->resize(size);
  return WriteNativeValue(integer, number, is_float, type, size,
      buffer->data());
}

// A single field update, resolved to its parameter/field location and
// already translated to its native representation, grouped and applied per
// parameter index by C2Module::SetExtraParams().
struct C2ParamFieldUpdate {
  uint32_t                      offset;
  uint32_t                      size;
  uint32_t                      extent;
  uint32_t                      base_size;
  C2FieldDescriptor::type_t     type;
  std::vector<uint8_t>          value;
};

c2_status_t C2Module::SetExtraParams(
    const std::vector<std::pair<std::string, std::any>>& fields) {

  if (fields.empty())
    return C2_OK;

  C2ParamFieldMap& fieldmap = GetFieldMap();

  // Group by parameter and field offset. A repeated field keeps its last value.
  std::map<uint32_t, std::map<uint32_t, C2ParamFieldUpdate>> updates;

  for (const auto& field : fields) {
    C2Param::Index index(0u);
    uint32_t offset = 0, size = 0, extent = 0, base_size = 0;
    C2FieldDescriptor::type_t type;

    if (!fieldmap.FindField(field.first, &index, &offset, &size, &type,
            &extent, &base_size) || (size == 0))
      continue;

    std::vector<uint8_t> value;
    if (!AnyToBuffer(field.second, type, size, &value) ||
        (value.size() % size != 0) ||
        (value.size() / size > std::numeric_limits<uint32_t>::max()))
      continue;

    uint32_t count = static_cast<uint32_t>(value.size() / size);
    if ((extent != 0) && (count > extent))
      continue;

    updates[static_cast<uint32_t>(index)][offset] = {
        offset, size, extent, base_size, type, std::move(value),
    };
  }

  if (updates.empty())
    return C2_OK;

  c2_status_t result = C2_OK;

  for (const auto& entry : updates) {
    C2Param::Index index(entry.first);

    try {
      // Query the current value, so that fields which are not being set keep
      // their value once the parameter is configured back.
      std::unique_ptr<C2Param> param = QueryParam(index);
      if (param == nullptr) {
        result = C2_CORRUPTED;
        continue;
      }

      // A C2 flexible parameter has one trailing flexible field. Resize once
      // to BASE_SIZE + FLEX_SIZE * count, matching C2ParamDef.h exactly.
      uint32_t target_size = static_cast<uint32_t>(param->size());
      bool valid_size = true;

      for (const auto& field_update : entry.second) {
        const C2ParamFieldUpdate& update = field_update.second;
        if (update.extent != 0)
          continue;

        if (!AddWithoutOverflow(update.base_size, update.value.size(),
                &target_size)) {
          valid_size = false;
          break;
        }
      }

      if (!valid_size) {
        result = C2_BAD_VALUE;
        continue;
      }

      if (target_size != param->size()) {
        param = C2ParamFieldMap::AllocParam(index, target_size, param.get());
        if (param == nullptr) {
          result = C2_NO_MEMORY;
          continue;
        }
      }

      uint32_t applied = 0;

      for (const auto& field_update : entry.second) {
        const C2ParamFieldUpdate& update = field_update.second;
        if (param->size() < update.base_size) {
          result = C2_BAD_VALUE;
          continue;
        }

        uint64_t capacity = update.extent == 0
            ? param->size() - update.base_size
            : static_cast<uint64_t>(update.extent) * update.size;
        uint64_t end = static_cast<uint64_t>(update.offset) + update.value.size();
        uint64_t capacity_end = static_cast<uint64_t>(update.offset) + capacity;

        if ((update.value.size() > capacity) || (end > param->size()) ||
            (capacity_end > param->size())) {
          result = C2_BAD_VALUE;
          continue;
        }

        uint8_t *dst = reinterpret_cast<uint8_t*>(param.get()) + update.offset;
        if (update.type == C2FieldDescriptor::STRING)
          memset(dst, 0, capacity);
        if (!update.value.empty())
          memcpy(dst, update.value.data(), update.value.size());

        applied++;
      }

      if (applied == 0)
        continue;

      SetParam(param);
    } catch (std::exception&) {
      result = C2_CORRUPTED;
    }
  }

  return result;
}

std::vector<std::shared_ptr<C2ParamDescriptor>>
C2Module::QuerySupportedParams() {

  std::vector<std::shared_ptr<C2ParamDescriptor>> descriptors;
  auto status = interface_->querySupportedParams_nb(&descriptors);

  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to query supported parameters, error ", status, "!");
  }

  return descriptors;
}

c2_status_t C2Module::QuerySupportedValues(
    std::vector<C2FieldSupportedValuesQuery>& fields) {

  auto status = interface_->querySupportedValues_vb(fields, C2_MAY_BLOCK);

  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to query supported values, error ", status, "!");
  }

  return C2_OK;
}

std::shared_ptr<C2ParamReflector> C2Module::GetParamReflector() {

  auto reflector = store_->getParamReflector();
  if (!reflector) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to get parameter reflector!");
  }

  return reflector;
}

C2ParamFieldMap& C2Module::GetFieldMap() {

  std::call_once(fieldmap_once_, [this]() {
    auto fieldmap = std::make_unique<C2ParamFieldMap>(GetParamReflector());
    fieldmap->AddParamDescriptors(QuerySupportedParams());
    fieldmap_ = std::move(fieldmap);
  });

  return *fieldmap_;
}

c2_status_t C2Module::Start() {

  std::lock_guard<std::mutex> lk(lock_);

  if (state_ == State::kCreated) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Start failed! Not initialized!");
  } else if (state_ == State::kError) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Start failed! Codec2 is in error state!");
  } else if (state_ == State::kRunning) {
    return C2_OK;
  }

  auto status = component_->start();
  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Start failed, error ", status, "!");
  }

  state_ = State::kRunning;
  return C2_OK;
}

c2_status_t C2Module::Stop() {

  std::lock_guard<std::mutex> lk(lock_);

  if (state_ == State::kCreated) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Stop failed! Not initialized!");
  } else if (state_ == State::kError) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Stop failed! Codec2 is in error state!");
  } else if (state_ == State::kIdle) {
    return C2_OK;
  }

  auto status = component_->stop();
  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Stop failed, error ", status, "!");
  }

  state_ = State::kIdle;
  return C2_OK;
}

c2_status_t C2Module::Flush(C2Component::flush_mode_t mode) {

  std::lock_guard<std::mutex> lk(lock_);

  if (state_ == State::kCreated) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Flush failed! Not initialized!");
  } else if (state_ == State::kError) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Flush failed! Codec2 is in error state!");
  } else if (state_ == State::kIdle) {
    return C2_OK;
  }

  std::list<std::unique_ptr<C2Work>> witems;
  auto status = component_->flush_sm(mode, &witems);

  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Flush failed, error ", status, "!");
  }

  HandleWorkDone(std::move(witems));
  return C2_OK;
}

c2_status_t C2Module::Drain(C2Component::drain_mode_t mode) {

  std::lock_guard<std::mutex> lk(lock_);

  if (state_ == State::kCreated) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Drain failed! Not initialized!");
  } else if (state_ == State::kError) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Drain failed! Codec2 is in error state!");
  } else if (state_ == State::kIdle) {
    return C2_OK;
  }

  auto status = component_->drain_nb(mode);
  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Drain failed, error ", status, "!");
  }

  return C2_OK;
}

c2_status_t C2Module::Queue(std::shared_ptr<C2Buffer>& buffer,
                                  std::list<std::unique_ptr<C2Param>>& settings,
                                  uint64_t index, uint64_t timestamp,
                                  uint32_t flags) {

  std::lock_guard<std::mutex> lk(lock_);

  if (state_ == State::kCreated) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Queue failed! Not initialized!");
  } else if (state_ == State::kError) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Queue failed! Codec2 is in error state!");
  } else if (state_ != State::kRunning) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Queue failed! Not in running state!");
  }

  std::unique_ptr<C2Work> work = std::make_unique<C2Work> ();

  work->input.ordinal.frameIndex = index;
  work->input.ordinal.timestamp = timestamp;
  work->input.flags = static_cast<C2FrameData::flags_t>(flags);
  work->input.buffers.emplace_back(buffer);

  auto worklet = std::make_unique<C2Worklet>();

  for (; !settings.empty(); settings.pop_front()) {
    std::unique_ptr<C2Param>& param = settings.front();
    worklet->tunings.push_back(
        std::unique_ptr<C2Tuning>(reinterpret_cast<C2Tuning*>(param.release())));
  }

  work->worklets.emplace_back(std::move(worklet));

  std::list<std::unique_ptr<C2Work>> witems;
  witems.push_back(std::move(work));

  auto status = component_->queue_nb (&witems);
  if (status != C2_OK) {
    throw Exception("Component[", interface_->getName().c_str(), "]: "
        "Failed to queue work items, error ", status, "!");
  }

  return C2_OK;
}

void C2Module::HandleWorkDone(std::list<std::unique_ptr<C2Work>> witems) {

  while (!witems.empty()) {
    std::unique_ptr<C2Work> work = std::move(witems.front());
    witems.pop_front();

    if (!work || work->worklets.empty()) {
      // No work item or empty worklets, skip.
      continue;
    }

    const std::unique_ptr<C2Worklet>& worklet = work->worklets.front();
    C2FrameData::flags_t flags = worklet->output.flags;

    if (flags & C2FrameData::FLAG_END_OF_STREAM) {
      notifier_->EventHandler(C2EventType::kEOS, nullptr);
      continue;
    }

    if (flags & C2FrameData::FLAG_DROP_FRAME ||
        flags & C2FrameData::FLAG_DISCARD_FRAME ||
        (worklet->output.buffers.empty() && (flags == 0))) {
      uint64_t index = worklet->output.ordinal.frameIndex.peeku();
      notifier_->EventHandler(C2EventType::kDrop, &index);
      continue;
    }

    // Process the worklets.
    if (work->workletsProcessed > 0 && !worklet->output.buffers.empty()) {
      auto buffer = worklet->output.buffers[0];
      uint64_t index = worklet->output.ordinal.frameIndex.peeku();
      uint64_t timestamp = worklet->output.ordinal.timestamp.peeku();

      notifier_->FrameAvailable(buffer, index, timestamp, flags);
    }
  }
}

void C2Module::HandleTripped(
    std::vector<std::shared_ptr<C2SettingResult>> results) {

  // Not implemented.
}

void C2Module::HandleError(uint32_t error) {
  state_ = State::kError;
  notifier_->EventHandler(C2EventType::kError, &error);
}

C2Module* C2Factory::GetModule(std::string name, C2ModeType mode) {

  std::lock_guard<std::mutex> lk(C2Factory::lock_);

  bool is_audio =
      mode == C2ModeType::kAudioEncode ||
      mode == C2ModeType::kAudioDecode;

  // Initialize Codec2 Store Factory.
  if ((is_audio && !factory_audio_) || (!is_audio && !factory_video_)) {
    const char* dll_lib = is_audio ?
        "libqc2audio_core.so" :
        "libqcodec2_core.so";
    const char* method = is_audio ?
        "QC2AudioComponentStoreFactoryGetter" :
        "QC2ComponentStoreFactoryGetter";

    void* handle = dlopen(dll_lib, RTLD_NOW);
    if (!handle) {
      throw std::runtime_error("dlopen failed, error: " + std::string(dlerror()));
    }

    auto FactoryGetter = (QC2ComponentStoreFactoryGetter_t)dlsym(handle, method);

    if ((FactoryGetter == nullptr)) {
      dlclose(handle);
      throw std::runtime_error("dlsym failed, error: " + std::string(dlerror()));
    }

    // Get version 1.0 of the Codec2 Store Factory.
    QC2ComponentStoreFactory* sfactory = (*FactoryGetter) (1, 0);
    if (sfactory == nullptr) {
      dlclose(handle);
      throw std::runtime_error("Unable to fetch Codec2 Store Factory!");
    }

    auto factory = std::shared_ptr<QC2ComponentStoreFactory>(sfactory,
        [handle](QC2ComponentStoreFactory* factory) {
          delete factory;
          dlclose(handle);
        }
    );
    if (is_audio) {
      factory_audio_ = factory;
    } else {
      factory_video_ = factory;
    }
  }

  // Fetch an instance of the Codec2 store.
  std::shared_ptr<C2ComponentStore> store =
      is_audio ? factory_audio_->getInstance() : factory_video_->getInstance();
  if (!store) {
    throw std::runtime_error("Unable to get Codec2 Store!");
  }

  // Create Codec2 component with the given name.
  std::shared_ptr<C2Component> component;
  auto status = store->createComponent(name, &component);

  if (status != C2_OK) {
    throw Exception("Unable to create Codec2 component '", name,
        "', error: ", status, " !");
  }

  return new C2Module(component, store, mode);
}
