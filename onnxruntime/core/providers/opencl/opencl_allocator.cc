// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/graph/constants.h"
#include "opencl_allocator.h"
#include "opencl_utils.h"

#include <iostream>

namespace onnxruntime {
namespace opencl {

OpenCLBufferAllocator::OpenCLBufferAllocator(cl_context ctx)
    : IAllocator(
          OrtMemoryInfo(
              BufferAllocatorName,
              OrtAllocatorType::OrtDeviceAllocator,
              OrtDevice(OrtDevice::GPU, CLMemType::OPENCL_BUFFER, /*device_id_=*/0),
              /*id_*/ 0,
              /* We blindly cast an integer CLMemType::OPENCL_BUFFER to a C style enum OrtMemType here. Because we
                 don't want to extend the OrtMemType enum at the moment, as it is in public interface header.
                 We manage allocator fully by at EP level so it does not go through AllocatorManager, thus we don't
                 need to worry about the magic value collide with existing value. */
              /*mem_type_=*/static_cast<OrtMemType>(CLMemType::OPENCL_BUFFER))),
      ctx_(ctx) {
}

OpenCLBufferAllocator::~OpenCLBufferAllocator() {
  for (auto& [ptr, _] : meta_) {
    ORT_UNUSED_PARAMETER(_);
    clReleaseMemObject(reinterpret_cast<cl_mem>(ptr));
  }
}

void* OpenCLBufferAllocator::Alloc(size_t size) {
  ZoneScopedN("OpenCLBufferAllocator::Alloc");
  auto it = cache_.find(size);

  if (it == cache_.end() || it->second.empty()) {
    cl_int err{};
    auto* ptr = clCreateBuffer(ctx_, CL_MEM_READ_WRITE, size, nullptr, &err);
    ORT_THROW_IF_CL_ERROR(err);
    VLOGF_DEFAULT(V_ALLOC, "Allocated Buffer(%p){size=%zu}", ptr, size);
    meta_[ptr] = {size};
    return ptr;
  }

  auto* ptr = it->second.front();
  VLOGF_DEFAULT(V_ALLOC, "Reused %p", ptr);
  it->second.pop_front();
  return ptr;
}

void OpenCLBufferAllocator::Free(void* p) {
  auto meta = meta_[p];
  auto it = cache_.find(meta.size);
  if (it == cache_.end()) {
    it = cache_.insert({meta.size, {}}).first;
  }
  it->second.push_front(p);
}

OpenCLImage2DAllocator::OpenCLImage2DAllocator(cl_context ctx, bool use_fp16, size_t* device_image_wh_limit)
    : IAllocator(OrtMemoryInfo(Image2DAllocatorName, OrtAllocatorType::OrtDeviceAllocator,
                               OrtDevice(OrtDevice::GPU, CLMemType::OPENCL_IMAGE_2D, /*device_id_=*/0))),
      ctx_(ctx),
      use_fp16_{use_fp16} {
  image_max_wh[0] = device_image_wh_limit[0];  // width
  image_max_wh[1] = device_image_wh_limit[1];  // height
}

OpenCLImage2DAllocator::~OpenCLImage2DAllocator() {
  for (auto& [ptr, _] : meta_) {
    ORT_UNUSED_PARAMETER(_);
    clReleaseMemObject(reinterpret_cast<cl_mem>(ptr));
  }
}

void* OpenCLImage2DAllocator::Alloc(size_t) {
  // not supported
  return nullptr;
}

void* OpenCLImage2DAllocator::Alloc(const TensorShape& shape) {
  return Alloc(Image2DDesc::PackFromTensor(shape));
}

void* OpenCLImage2DAllocator::Alloc(const Image2DDesc& desc) {
  ZoneScopedN("OpenCLImage2DAllocator::Alloc");
  auto it = cache_.find(desc);
  if (it == cache_.end() || it->second.empty()) {
    cl_int err{};
    ORT_ENFORCE(desc.Height() > 0 && desc.Height() <= image_max_wh[1], "Image2D height invalid");
    ORT_ENFORCE(desc.Width() > 0 && desc.Width() <= image_max_wh[0], "Image2D width invalid");
    cl_image_format image_format;
    image_format.image_channel_data_type = use_fp16_ ? CL_HALF_FLOAT : CL_FLOAT;
    image_format.image_channel_order = CL_RGBA;
    cl_image_desc image_desc;
    {
      image_desc.image_type = CL_MEM_OBJECT_IMAGE2D;
      image_desc.image_width = desc.UWidth();
      image_desc.image_height = desc.UHeight();
      image_desc.image_depth = 0;        // unused
      image_desc.image_array_size = 0;   // unused
      image_desc.image_row_pitch = 0;    // must be 0 if host_ptr is nullptr
      image_desc.image_slice_pitch = 0;  // must be 0 if host_ptr is nullptr
      image_desc.num_mip_levels = 0;     // must be 0
      image_desc.num_samples = 0;        // must be 0
      image_desc.buffer = nullptr;
    }
    auto* ptr = clCreateImage(ctx_, CL_MEM_READ_WRITE, &image_format, &image_desc, nullptr, &err);
    ORT_THROW_IF_CL_ERROR(err);
    VLOGF_DEFAULT(V_ALLOC, "Allocated Image2D(%p){w=%ld, h=%ld})", ptr, desc.Width(), desc.Height());
    meta_[ptr] = {desc};
    return ptr;
  }

  auto* ptr = it->second.front();
  VLOGF_DEFAULT(V_ALLOC, "Reused %p", ptr);
  it->second.pop_front();
  return ptr;
}

void OpenCLImage2DAllocator::Free(void* p) {
  auto meta = meta_[p];
  auto it = cache_.find(meta.desc);
  if (it == cache_.end()) {
    it = cache_.insert({meta.desc, {}}).first;
  }
  it->second.push_front(p);
}

}  // namespace opencl
}  // namespace onnxruntime
