/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#include "vn_device.h"

#include <stdio.h>

#include "git_sha1.h"
#include "util/driconf.h"
#include "util/mesa-sha1.h"
#include "venus-protocol/vn_protocol_driver.h"

#include "vn_icd.h"
#include "vn_renderer.h"

/* require and request at least Vulkan 1.1 at both instance and device levels
 */
#define VN_MIN_RENDERER_VERSION VK_API_VERSION_1_1

/*
 * Instance extensions add instance-level or physical-device-level
 * functionalities.  It seems renderer support is either unnecessary or
 * optional.  We should be able to advertise them or lie about them locally.
 */
static const struct vk_instance_extension_table
   vn_instance_supported_extensions;

static const driOptionDescription vn_dri_options[] = {
   /* clang-format off */
   DRI_CONF_SECTION_PERFORMANCE
      DRI_CONF_VK_X11_ENSURE_MIN_IMAGE_COUNT(false)
      DRI_CONF_VK_X11_OVERRIDE_MIN_IMAGE_COUNT(0)
      DRI_CONF_VK_X11_STRICT_IMAGE_COUNT(false)
   DRI_CONF_SECTION_END
   DRI_CONF_SECTION_DEBUG
      DRI_CONF_VK_WSI_FORCE_BGRA8_UNORM_FIRST(false)
   DRI_CONF_SECTION_END
   /* clang-format on */
};

static VkResult
vn_instance_init_version(struct vn_instance *instance)
{
   uint32_t renderer_version = 0;
   VkResult result =
      vn_call_vkEnumerateInstanceVersion(instance, &renderer_version);
   if (result != VK_SUCCESS) {
      if (VN_DEBUG(INIT))
         vn_log(instance, "failed to enumerate renderer instance version");
      return result;
   }

   if (renderer_version < VN_MIN_RENDERER_VERSION) {
      if (VN_DEBUG(INIT)) {
         vn_log(instance, "unsupported renderer instance version %d.%d",
                VK_VERSION_MAJOR(instance->renderer_version),
                VK_VERSION_MINOR(instance->renderer_version));
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   instance->renderer_version =
      instance->base.base.app_info.api_version > VN_MIN_RENDERER_VERSION
         ? instance->base.base.app_info.api_version
         : VN_MIN_RENDERER_VERSION;

   if (VN_DEBUG(INIT)) {
      vn_log(instance, "vk instance version %d.%d.%d",
             VK_VERSION_MAJOR(instance->renderer_version),
             VK_VERSION_MINOR(instance->renderer_version),
             VK_VERSION_PATCH(instance->renderer_version));
   }

   return VK_SUCCESS;
}

static VkResult
vn_instance_init_ring(struct vn_instance *instance)
{
   /* 32-bit seqno for renderer roundtrips */
   const size_t extra_size = sizeof(uint32_t);
   struct vn_ring_layout layout;
   vn_ring_get_layout(extra_size, &layout);

   void *ring_ptr;
   VkResult result = vn_renderer_bo_create_cpu(
      instance->renderer, layout.bo_size, &instance->ring.bo);
   if (result == VK_SUCCESS) {
      ring_ptr = vn_renderer_bo_map(instance->ring.bo);
      if (!ring_ptr)
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   if (result != VK_SUCCESS) {
      if (VN_DEBUG(INIT))
         vn_log(instance, "failed to allocate/map ring bo");
      return result;
   }

   mtx_init(&instance->ring.mutex, mtx_plain);

   struct vn_ring *ring = &instance->ring.ring;
   vn_ring_init(ring, &layout, ring_ptr);

   instance->ring.id = (uintptr_t)ring;

   const struct VkRingCreateInfoMESA info = {
      .sType = VK_STRUCTURE_TYPE_RING_CREATE_INFO_MESA,
      .resourceId = instance->ring.bo->res_id,
      .size = layout.bo_size,
      .idleTimeout = 50ull * 1000 * 1000,
      .headOffset = layout.head_offset,
      .tailOffset = layout.tail_offset,
      .statusOffset = layout.status_offset,
      .bufferOffset = layout.buffer_offset,
      .bufferSize = layout.buffer_size,
      .extraOffset = layout.extra_offset,
      .extraSize = layout.extra_size,
   };

   uint32_t create_ring_data[64];
   struct vn_cs_encoder local_enc =
      VN_CS_ENCODER_INITIALIZER(create_ring_data, sizeof(create_ring_data));
   vn_encode_vkCreateRingMESA(&local_enc, 0, instance->ring.id, &info);
   vn_renderer_submit_simple(instance->renderer, create_ring_data,
                             vn_cs_encoder_get_len(&local_enc));

   vn_cs_encoder_init_indirect(&instance->ring.upload, instance,
                               1 * 1024 * 1024);

   return VK_SUCCESS;
}

static VkResult
vn_instance_init_renderer(struct vn_instance *instance)
{
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   VkResult result = vn_renderer_create(instance, alloc, &instance->renderer);
   if (result != VK_SUCCESS)
      return result;

   mtx_init(&instance->roundtrip_mutex, mtx_plain);
   instance->roundtrip_next = 1;

   vn_renderer_get_info(instance->renderer, &instance->renderer_info);

   uint32_t version = vn_info_wire_format_version();
   if (instance->renderer_info.wire_format_version != version) {
      if (VN_DEBUG(INIT)) {
         vn_log(instance, "wire format version %d != %d",
                instance->renderer_info.wire_format_version, version);
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   version = vn_info_vk_xml_version();
   if (instance->renderer_info.vk_xml_version > version)
      instance->renderer_info.vk_xml_version = version;

   version = vn_info_extension_spec_version("VK_EXT_command_serialization");
   if (instance->renderer_info.vk_ext_command_serialization_spec_version >
       version) {
      instance->renderer_info.vk_ext_command_serialization_spec_version =
         version;
   }

   version = vn_info_extension_spec_version("VK_MESA_venus_protocol");
   if (instance->renderer_info.vk_mesa_venus_protocol_spec_version >
       version) {
      instance->renderer_info.vk_mesa_venus_protocol_spec_version = version;
   }

   if (VN_DEBUG(INIT)) {
      vn_log(instance, "connected to renderer");
      vn_log(instance, "wire format version %d",
             instance->renderer_info.wire_format_version);
      vn_log(instance, "vk xml version %d.%d.%d",
             VK_VERSION_MAJOR(instance->renderer_info.vk_xml_version),
             VK_VERSION_MINOR(instance->renderer_info.vk_xml_version),
             VK_VERSION_PATCH(instance->renderer_info.vk_xml_version));
      vn_log(
         instance, "VK_EXT_command_serialization spec version %d",
         instance->renderer_info.vk_ext_command_serialization_spec_version);
      vn_log(instance, "VK_MESA_venus_protocol spec version %d",
             instance->renderer_info.vk_mesa_venus_protocol_spec_version);
   }

   return VK_SUCCESS;
}

VkResult
vn_instance_submit_roundtrip(struct vn_instance *instance,
                             uint32_t *roundtrip_seqno)
{
   uint32_t write_ring_extra_data[8];
   struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
      write_ring_extra_data, sizeof(write_ring_extra_data));

   /* submit a vkWriteRingExtraMESA through the renderer */
   mtx_lock(&instance->roundtrip_mutex);
   const uint32_t seqno = instance->roundtrip_next++;
   vn_encode_vkWriteRingExtraMESA(&local_enc, 0, instance->ring.id, 0, seqno);
   VkResult result =
      vn_renderer_submit_simple(instance->renderer, write_ring_extra_data,
                                vn_cs_encoder_get_len(&local_enc));
   mtx_unlock(&instance->roundtrip_mutex);

   *roundtrip_seqno = seqno;
   return result;
}

static void
vn_instance_wait_roundtrip(struct vn_instance *instance,
                           uint32_t roundtrip_seqno)
{
   const struct vn_ring *ring = &instance->ring.ring;
   const volatile atomic_uint *ptr = ring->shared.extra;
   uint32_t iter = 0;
   do {
      const uint32_t cur = atomic_load_explicit(ptr, memory_order_acquire);
      if (cur >= roundtrip_seqno || roundtrip_seqno - cur >= INT32_MAX)
         break;
      vn_relax(&iter);
   } while (true);
}

static void
vn_instance_roundtrip(struct vn_instance *instance)
{
   uint32_t roundtrip_seqno;
   if (vn_instance_submit_roundtrip(instance, &roundtrip_seqno) == VK_SUCCESS)
      vn_instance_wait_roundtrip(instance, roundtrip_seqno);
}

struct vn_instance_submission {
   uint32_t local_cs_data[64];

   void *cs_data;
   size_t cs_size;
   struct vn_ring_submit *submit;
};

static void *
vn_instance_submission_indirect_cs(struct vn_instance_submission *submit,
                                   const struct vn_cs_encoder *cs,
                                   size_t *cs_size)
{
   VkCommandStreamDescriptionMESA local_descs[8];
   VkCommandStreamDescriptionMESA *descs = local_descs;
   if (cs->buffer_count > ARRAY_SIZE(local_descs)) {
      descs =
         malloc(sizeof(VkCommandStreamDescriptionMESA) * cs->buffer_count);
      if (!descs)
         return NULL;
   }

   uint32_t desc_count = 0;
   for (uint32_t i = 0; i < cs->buffer_count; i++) {
      const struct vn_cs_encoder_buffer *buf = &cs->buffers[i];
      if (buf->committed_size) {
         descs[desc_count++] = (VkCommandStreamDescriptionMESA){
            .resourceId = buf->bo->res_id,
            .offset = buf->offset,
            .size = buf->committed_size,
         };
      }
   }

   const size_t exec_size = vn_sizeof_vkExecuteCommandStreamsMESA(
      desc_count, descs, NULL, 0, NULL, 0);
   void *exec_data = submit->local_cs_data;
   if (exec_size > sizeof(submit->local_cs_data)) {
      exec_data = malloc(exec_size);
      if (!exec_data)
         goto out;
   }

   struct vn_cs_encoder local_enc =
      VN_CS_ENCODER_INITIALIZER(exec_data, exec_size);
   vn_encode_vkExecuteCommandStreamsMESA(&local_enc, 0, desc_count, descs,
                                         NULL, 0, NULL, 0);

   *cs_size = vn_cs_encoder_get_len(&local_enc);

out:
   if (descs != local_descs)
      free(descs);

   return exec_data;
}

static void *
vn_instance_submission_direct_cs(struct vn_instance_submission *submit,
                                 const struct vn_cs_encoder *cs,
                                 size_t *cs_size)
{
   if (cs->buffer_count == 1) {
      *cs_size = cs->buffers[0].committed_size;
      return cs->buffers[0].base;
   }

   assert(vn_cs_encoder_get_len(cs) <= sizeof(submit->local_cs_data));
   void *dst = submit->local_cs_data;
   for (uint32_t i = 0; i < cs->buffer_count; i++) {
      const struct vn_cs_encoder_buffer *buf = &cs->buffers[i];
      memcpy(dst, buf->base, buf->committed_size);
      dst += buf->committed_size;
   }

   *cs_size = dst - (void *)submit->local_cs_data;
   return submit->local_cs_data;
}

static struct vn_ring_submit *
vn_instance_submission_get_ring_submit(struct vn_ring *ring,
                                       const struct vn_cs_encoder *cs,
                                       struct vn_renderer_bo *extra_bo,
                                       bool direct)
{
   const uint32_t bo_count =
      (direct ? 0 : cs->buffer_count) + (extra_bo ? 1 : 0);
   struct vn_ring_submit *submit = vn_ring_get_submit(ring, bo_count);
   if (!submit)
      return NULL;

   submit->bo_count = bo_count;
   if (!direct) {
      for (uint32_t i = 0; i < cs->buffer_count; i++)
         submit->bos[i] = vn_renderer_bo_ref(cs->buffers[i].bo);
   }
   if (extra_bo)
      submit->bos[bo_count - 1] = vn_renderer_bo_ref(extra_bo);

   return submit;
}

static void
vn_instance_submission_cleanup(struct vn_instance_submission *submit,
                               const struct vn_cs_encoder *cs)
{
   if (submit->cs_data != submit->local_cs_data &&
       submit->cs_data != cs->buffers[0].base)
      free(submit->cs_data);
}

static VkResult
vn_instance_submission_prepare(struct vn_instance_submission *submit,
                               const struct vn_cs_encoder *cs,
                               struct vn_ring *ring,
                               struct vn_renderer_bo *extra_bo,
                               bool direct)
{
   if (direct) {
      submit->cs_data =
         vn_instance_submission_direct_cs(submit, cs, &submit->cs_size);
   } else {
      submit->cs_data =
         vn_instance_submission_indirect_cs(submit, cs, &submit->cs_size);
   }
   if (!submit->cs_data)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   submit->submit =
      vn_instance_submission_get_ring_submit(ring, cs, extra_bo, direct);
   if (!submit->submit) {
      vn_instance_submission_cleanup(submit, cs);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   return VK_SUCCESS;
}

static bool
vn_instance_submission_can_direct(const struct vn_cs_encoder *cs)
{
   struct vn_instance_submission submit;
   return vn_cs_encoder_get_len(cs) <= sizeof(submit.local_cs_data);
}

static struct vn_cs_encoder *
vn_instance_ring_cs_upload_locked(struct vn_instance *instance,
                                  const struct vn_cs_encoder *cs)
{
   assert(!cs->indirect && cs->buffer_count == 1);
   const void *cs_data = cs->buffers[0].base;
   const size_t cs_size = cs->total_committed_size;
   assert(cs_size == vn_cs_encoder_get_len(cs));

   struct vn_cs_encoder *upload = &instance->ring.upload;
   vn_cs_encoder_reset(upload);

   if (!vn_cs_encoder_reserve(upload, cs_size))
      return NULL;

   vn_cs_encoder_write(upload, cs_size, cs_data, cs_size);
   vn_cs_encoder_commit(upload);
   vn_instance_wait_roundtrip(instance, upload->current_buffer_roundtrip);

   return upload;
}

static VkResult
vn_instance_ring_submit_locked(struct vn_instance *instance,
                               const struct vn_cs_encoder *cs,
                               struct vn_renderer_bo *extra_bo,
                               uint32_t *ring_seqno)
{
   struct vn_ring *ring = &instance->ring.ring;

   const bool direct = vn_instance_submission_can_direct(cs);
   if (!direct && !cs->indirect) {
      cs = vn_instance_ring_cs_upload_locked(instance, cs);
      if (!cs)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      assert(cs->indirect);
   }

   struct vn_instance_submission submit;
   VkResult result =
      vn_instance_submission_prepare(&submit, cs, ring, extra_bo, direct);
   if (result != VK_SUCCESS)
      return result;

   uint32_t seqno;
   const bool notify = vn_ring_submit(ring, submit.submit, submit.cs_data,
                                      submit.cs_size, &seqno);
   if (notify) {
      uint32_t notify_ring_data[8];
      struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
         notify_ring_data, sizeof(notify_ring_data));
      vn_encode_vkNotifyRingMESA(&local_enc, 0, instance->ring.id, seqno, 0);
      vn_renderer_submit_simple(instance->renderer, notify_ring_data,
                                vn_cs_encoder_get_len(&local_enc));
   }

   vn_instance_submission_cleanup(&submit, cs);

   if (ring_seqno)
      *ring_seqno = seqno;

   return VK_SUCCESS;
}

static void
vn_instance_ring_wait(struct vn_instance *instance)
{
   struct vn_ring *ring = &instance->ring.ring;
   vn_ring_wait_all(ring);
}

static bool
vn_instance_grow_reply_bo_locked(struct vn_instance *instance, size_t size)
{
   const size_t min_bo_size = 1 << 20;

   size_t bo_size = instance->reply.size ? instance->reply.size : min_bo_size;
   while (bo_size < size) {
      bo_size <<= 1;
      if (!bo_size)
         return false;
   }

   struct vn_renderer_bo *bo;
   VkResult result =
      vn_renderer_bo_create_cpu(instance->renderer, bo_size, &bo);
   if (result != VK_SUCCESS)
      return false;

   void *ptr = vn_renderer_bo_map(bo);
   if (!ptr) {
      vn_renderer_bo_unref(bo);
      return false;
   }

   if (instance->reply.bo)
      vn_renderer_bo_unref(instance->reply.bo);
   instance->reply.bo = bo;
   instance->reply.size = bo_size;
   instance->reply.used = 0;
   instance->reply.ptr = ptr;

   return true;
}

static struct vn_renderer_bo *
vn_instance_get_reply_bo_locked(struct vn_instance *instance,
                                size_t size,
                                void **ptr)
{
   if (unlikely(instance->reply.used + size > instance->reply.size)) {
      if (!vn_instance_grow_reply_bo_locked(instance, size))
         return NULL;

      uint32_t set_reply_command_stream_data[16];
      struct vn_cs_encoder local_enc =
         VN_CS_ENCODER_INITIALIZER(set_reply_command_stream_data,
                                   sizeof(set_reply_command_stream_data));
      const struct VkCommandStreamDescriptionMESA stream = {
         .resourceId = instance->reply.bo->res_id,
         .size = instance->reply.size,
      };
      vn_encode_vkSetReplyCommandStreamMESA(&local_enc, 0, &stream);
      vn_cs_encoder_commit(&local_enc);

      vn_instance_roundtrip(instance);
      vn_instance_ring_submit_locked(instance, &local_enc, NULL, NULL);
   }

   /* TODO avoid this seek command and go lock-free? */
   uint32_t seek_reply_command_stream_data[8];
   struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
      seek_reply_command_stream_data, sizeof(seek_reply_command_stream_data));
   const size_t offset = instance->reply.used;
   vn_encode_vkSeekReplyCommandStreamMESA(&local_enc, 0, offset);
   vn_cs_encoder_commit(&local_enc);
   vn_instance_ring_submit_locked(instance, &local_enc, NULL, NULL);

   *ptr = instance->reply.ptr + offset;
   instance->reply.used += size;

   return vn_renderer_bo_ref(instance->reply.bo);
}

void
vn_instance_submit_command(struct vn_instance *instance,
                           struct vn_instance_submit_command *submit)
{
   void *reply_ptr;
   submit->reply_bo = NULL;

   mtx_lock(&instance->ring.mutex);

   if (vn_cs_encoder_is_empty(&submit->command))
      goto fail;
   vn_cs_encoder_commit(&submit->command);

   if (submit->reply_size) {
      submit->reply_bo = vn_instance_get_reply_bo_locked(
         instance, submit->reply_size, &reply_ptr);
      if (!submit->reply_bo)
         goto fail;
   }

   uint32_t ring_seqno;
   VkResult result = vn_instance_ring_submit_locked(
      instance, &submit->command, submit->reply_bo, &ring_seqno);

   mtx_unlock(&instance->ring.mutex);

   submit->reply = VN_CS_DECODER_INITIALIZER(reply_ptr, submit->reply_size);

   if (submit->reply_size && result == VK_SUCCESS)
      vn_ring_wait(&instance->ring.ring, ring_seqno);

   return;

fail:
   instance->ring.command_dropped++;
   mtx_unlock(&instance->ring.mutex);
}

static struct vn_physical_device *
vn_instance_find_physical_device(struct vn_instance *instance,
                                 vn_object_id id)
{
   for (uint32_t i = 0; i < instance->physical_device_count; i++) {
      if (instance->physical_devices[i].base.id == id)
         return &instance->physical_devices[i];
   }
   return NULL;
}

static void
vn_physical_device_init_features(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   struct {
      /* Vulkan 1.1 */
      VkPhysicalDevice16BitStorageFeatures sixteen_bit_storage;
      VkPhysicalDeviceMultiviewFeatures multiview;
      VkPhysicalDeviceVariablePointersFeatures variable_pointers;
      VkPhysicalDeviceProtectedMemoryFeatures protected_memory;
      VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr_conversion;
      VkPhysicalDeviceShaderDrawParametersFeatures shader_draw_parameters;

      /* Vulkan 1.2 */
      VkPhysicalDevice8BitStorageFeatures eight_bit_storage;
      VkPhysicalDeviceShaderAtomicInt64Features shader_atomic_int64;
      VkPhysicalDeviceShaderFloat16Int8Features shader_float16_int8;
      VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing;
      VkPhysicalDeviceScalarBlockLayoutFeatures scalar_block_layout;
      VkPhysicalDeviceImagelessFramebufferFeatures imageless_framebuffer;
      VkPhysicalDeviceUniformBufferStandardLayoutFeatures
         uniform_buffer_standard_layout;
      VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures
         shader_subgroup_extended_types;
      VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures
         separate_depth_stencil_layouts;
      VkPhysicalDeviceHostQueryResetFeatures host_query_reset;
      VkPhysicalDeviceTimelineSemaphoreFeatures timeline_semaphore;
      VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address;
      VkPhysicalDeviceVulkanMemoryModelFeatures vulkan_memory_model;
   } local_feats;

   physical_dev->features.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
   if (physical_dev->renderer_version >= VK_API_VERSION_1_2) {
      physical_dev->features.pNext = &physical_dev->vulkan_1_1_features;

      physical_dev->vulkan_1_1_features.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
      physical_dev->vulkan_1_1_features.pNext =
         &physical_dev->vulkan_1_2_features;
      physical_dev->vulkan_1_2_features.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
      physical_dev->vulkan_1_2_features.pNext = NULL;
   } else {
      physical_dev->features.pNext = &local_feats.sixteen_bit_storage;

      local_feats.sixteen_bit_storage.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
      local_feats.sixteen_bit_storage.pNext = &local_feats.multiview;
      local_feats.multiview.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
      local_feats.multiview.pNext = &local_feats.variable_pointers;
      local_feats.variable_pointers.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES;
      local_feats.variable_pointers.pNext = &local_feats.protected_memory;
      local_feats.protected_memory.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES;
      local_feats.protected_memory.pNext =
         &local_feats.sampler_ycbcr_conversion;
      local_feats.sampler_ycbcr_conversion.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
      local_feats.sampler_ycbcr_conversion.pNext =
         &local_feats.shader_draw_parameters;
      local_feats.shader_draw_parameters.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
      local_feats.shader_draw_parameters.pNext =
         &local_feats.eight_bit_storage;

      local_feats.eight_bit_storage.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
      local_feats.eight_bit_storage.pNext = &local_feats.shader_atomic_int64;
      local_feats.shader_atomic_int64.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
      local_feats.shader_atomic_int64.pNext =
         &local_feats.shader_float16_int8;
      local_feats.shader_float16_int8.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
      local_feats.shader_float16_int8.pNext =
         &local_feats.descriptor_indexing;
      local_feats.descriptor_indexing.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
      local_feats.descriptor_indexing.pNext =
         &local_feats.scalar_block_layout;
      local_feats.scalar_block_layout.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
      local_feats.scalar_block_layout.pNext =
         &local_feats.imageless_framebuffer;
      local_feats.imageless_framebuffer.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES;
      local_feats.imageless_framebuffer.pNext =
         &local_feats.uniform_buffer_standard_layout;
      local_feats.uniform_buffer_standard_layout.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES;
      local_feats.uniform_buffer_standard_layout.pNext =
         &local_feats.shader_subgroup_extended_types;
      local_feats.shader_subgroup_extended_types.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES;
      local_feats.shader_subgroup_extended_types.pNext =
         &local_feats.separate_depth_stencil_layouts;
      local_feats.separate_depth_stencil_layouts.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES;
      local_feats.separate_depth_stencil_layouts.pNext =
         &local_feats.host_query_reset;
      local_feats.host_query_reset.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
      local_feats.host_query_reset.pNext = &local_feats.timeline_semaphore;
      local_feats.timeline_semaphore.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
      local_feats.timeline_semaphore.pNext =
         &local_feats.buffer_device_address;
      local_feats.buffer_device_address.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
      local_feats.buffer_device_address.pNext =
         &local_feats.vulkan_memory_model;
      local_feats.vulkan_memory_model.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
      local_feats.vulkan_memory_model.pNext = NULL;
   }

   vn_call_vkGetPhysicalDeviceFeatures2(
      instance, vn_physical_device_to_handle(physical_dev),
      &physical_dev->features);

   const struct vk_device_extension_table *exts =
      &physical_dev->renderer_extensions;
   struct VkPhysicalDeviceVulkan11Features *vk11_feats =
      &physical_dev->vulkan_1_1_features;
   struct VkPhysicalDeviceVulkan12Features *vk12_feats =
      &physical_dev->vulkan_1_2_features;

   if (physical_dev->renderer_version < VK_API_VERSION_1_2) {
      vk11_feats->storageBuffer16BitAccess =
         local_feats.sixteen_bit_storage.storageBuffer16BitAccess;
      vk11_feats->uniformAndStorageBuffer16BitAccess =
         local_feats.sixteen_bit_storage.uniformAndStorageBuffer16BitAccess;
      vk11_feats->storagePushConstant16 =
         local_feats.sixteen_bit_storage.storagePushConstant16;
      vk11_feats->storageInputOutput16 =
         local_feats.sixteen_bit_storage.storageInputOutput16;

      vk11_feats->multiview = local_feats.multiview.multiview;
      vk11_feats->multiviewGeometryShader =
         local_feats.multiview.multiviewGeometryShader;
      vk11_feats->multiviewTessellationShader =
         local_feats.multiview.multiviewTessellationShader;

      vk11_feats->variablePointersStorageBuffer =
         local_feats.variable_pointers.variablePointersStorageBuffer;
      vk11_feats->variablePointers =
         local_feats.variable_pointers.variablePointers;

      vk11_feats->protectedMemory =
         local_feats.protected_memory.protectedMemory;

      vk11_feats->samplerYcbcrConversion =
         local_feats.sampler_ycbcr_conversion.samplerYcbcrConversion;

      vk11_feats->shaderDrawParameters =
         local_feats.shader_draw_parameters.shaderDrawParameters;

      vk12_feats->samplerMirrorClampToEdge =
         exts->KHR_sampler_mirror_clamp_to_edge;
      vk12_feats->drawIndirectCount = exts->KHR_draw_indirect_count;

      if (exts->KHR_8bit_storage) {
         vk12_feats->storageBuffer8BitAccess =
            local_feats.eight_bit_storage.storageBuffer8BitAccess;
         vk12_feats->uniformAndStorageBuffer8BitAccess =
            local_feats.eight_bit_storage.uniformAndStorageBuffer8BitAccess;
         vk12_feats->storagePushConstant8 =
            local_feats.eight_bit_storage.storagePushConstant8;
      }
      if (exts->KHR_shader_atomic_int64) {
         vk12_feats->shaderBufferInt64Atomics =
            local_feats.shader_atomic_int64.shaderBufferInt64Atomics;
         vk12_feats->shaderSharedInt64Atomics =
            local_feats.shader_atomic_int64.shaderSharedInt64Atomics;
      }
      if (exts->KHR_shader_float16_int8) {
         vk12_feats->shaderFloat16 =
            local_feats.shader_float16_int8.shaderFloat16;
         vk12_feats->shaderInt8 = local_feats.shader_float16_int8.shaderInt8;
      }
      if (exts->EXT_descriptor_indexing) {
         vk12_feats->descriptorIndexing = true;
         vk12_feats->shaderInputAttachmentArrayDynamicIndexing =
            local_feats.descriptor_indexing
               .shaderInputAttachmentArrayDynamicIndexing;
         vk12_feats->shaderUniformTexelBufferArrayDynamicIndexing =
            local_feats.descriptor_indexing
               .shaderUniformTexelBufferArrayDynamicIndexing;
         vk12_feats->shaderStorageTexelBufferArrayDynamicIndexing =
            local_feats.descriptor_indexing
               .shaderStorageTexelBufferArrayDynamicIndexing;
         vk12_feats->shaderUniformBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderUniformBufferArrayNonUniformIndexing;
         vk12_feats->shaderSampledImageArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderSampledImageArrayNonUniformIndexing;
         vk12_feats->shaderStorageBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderStorageBufferArrayNonUniformIndexing;
         vk12_feats->shaderStorageImageArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderStorageImageArrayNonUniformIndexing;
         vk12_feats->shaderInputAttachmentArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderInputAttachmentArrayNonUniformIndexing;
         vk12_feats->shaderUniformTexelBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderUniformTexelBufferArrayNonUniformIndexing;
         vk12_feats->shaderStorageTexelBufferArrayNonUniformIndexing =
            local_feats.descriptor_indexing
               .shaderStorageTexelBufferArrayNonUniformIndexing;
         vk12_feats->descriptorBindingUniformBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingUniformBufferUpdateAfterBind;
         vk12_feats->descriptorBindingSampledImageUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingSampledImageUpdateAfterBind;
         vk12_feats->descriptorBindingStorageImageUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingStorageImageUpdateAfterBind;
         vk12_feats->descriptorBindingStorageBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingStorageBufferUpdateAfterBind;
         vk12_feats->descriptorBindingUniformTexelBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingUniformTexelBufferUpdateAfterBind;
         vk12_feats->descriptorBindingStorageTexelBufferUpdateAfterBind =
            local_feats.descriptor_indexing
               .descriptorBindingStorageTexelBufferUpdateAfterBind;
         vk12_feats->descriptorBindingUpdateUnusedWhilePending =
            local_feats.descriptor_indexing
               .descriptorBindingUpdateUnusedWhilePending;
         vk12_feats->descriptorBindingPartiallyBound =
            local_feats.descriptor_indexing.descriptorBindingPartiallyBound;
         vk12_feats->descriptorBindingVariableDescriptorCount =
            local_feats.descriptor_indexing
               .descriptorBindingVariableDescriptorCount;
         vk12_feats->runtimeDescriptorArray =
            local_feats.descriptor_indexing.runtimeDescriptorArray;
      }

      vk12_feats->samplerFilterMinmax = exts->EXT_sampler_filter_minmax;

      if (exts->EXT_scalar_block_layout) {
         vk12_feats->scalarBlockLayout =
            local_feats.scalar_block_layout.scalarBlockLayout;
      }
      if (exts->KHR_imageless_framebuffer) {
         vk12_feats->imagelessFramebuffer =
            local_feats.imageless_framebuffer.imagelessFramebuffer;
      }
      if (exts->KHR_uniform_buffer_standard_layout) {
         vk12_feats->uniformBufferStandardLayout =
            local_feats.uniform_buffer_standard_layout
               .uniformBufferStandardLayout;
      }
      if (exts->KHR_shader_subgroup_extended_types) {
         vk12_feats->shaderSubgroupExtendedTypes =
            local_feats.shader_subgroup_extended_types
               .shaderSubgroupExtendedTypes;
      }
      if (exts->KHR_separate_depth_stencil_layouts) {
         vk12_feats->separateDepthStencilLayouts =
            local_feats.separate_depth_stencil_layouts
               .separateDepthStencilLayouts;
      }
      if (exts->EXT_host_query_reset) {
         vk12_feats->hostQueryReset =
            local_feats.host_query_reset.hostQueryReset;
      }
      if (exts->KHR_timeline_semaphore) {
         vk12_feats->timelineSemaphore =
            local_feats.timeline_semaphore.timelineSemaphore;
      }
      if (exts->KHR_buffer_device_address) {
         vk12_feats->bufferDeviceAddress =
            local_feats.buffer_device_address.bufferDeviceAddress;
         vk12_feats->bufferDeviceAddressCaptureReplay =
            local_feats.buffer_device_address.bufferDeviceAddressCaptureReplay;
         vk12_feats->bufferDeviceAddressMultiDevice =
            local_feats.buffer_device_address.bufferDeviceAddressMultiDevice;
      }
      if (exts->KHR_vulkan_memory_model) {
         vk12_feats->vulkanMemoryModel =
            local_feats.vulkan_memory_model.vulkanMemoryModel;
         vk12_feats->vulkanMemoryModelDeviceScope =
            local_feats.vulkan_memory_model.vulkanMemoryModelDeviceScope;
         vk12_feats->vulkanMemoryModelAvailabilityVisibilityChains =
            local_feats.vulkan_memory_model
               .vulkanMemoryModelAvailabilityVisibilityChains;
      }

      vk12_feats->shaderOutputViewportIndex =
         exts->EXT_shader_viewport_index_layer;
      vk12_feats->shaderOutputLayer = exts->EXT_shader_viewport_index_layer;
      vk12_feats->subgroupBroadcastDynamicId = false;
   }
}

static void
vn_physical_device_init_uuids(struct vn_physical_device *physical_dev)
{
   struct VkPhysicalDeviceProperties *props =
      &physical_dev->properties.properties;
   struct VkPhysicalDeviceVulkan11Properties *vk11_props =
      &physical_dev->vulkan_1_1_properties;
   struct VkPhysicalDeviceVulkan12Properties *vk12_props =
      &physical_dev->vulkan_1_2_properties;
   struct mesa_sha1 sha1_ctx;
   uint8_t sha1[SHA1_DIGEST_LENGTH];

   static_assert(VK_UUID_SIZE <= SHA1_DIGEST_LENGTH, "");

   /* keep props->pipelineCacheUUID? */

   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, &props->vendorID, sizeof(props->vendorID));
   _mesa_sha1_update(&sha1_ctx, &props->deviceID, sizeof(props->deviceID));
   _mesa_sha1_final(&sha1_ctx, sha1);

   memcpy(vk11_props->deviceUUID, sha1, VK_UUID_SIZE);

   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, vk12_props->driverName,
                     strlen(vk12_props->driverName));
   _mesa_sha1_update(&sha1_ctx, vk12_props->driverInfo,
                     strlen(vk12_props->driverInfo));
   _mesa_sha1_final(&sha1_ctx, sha1);

   memcpy(vk11_props->driverUUID, sha1, VK_UUID_SIZE);

   memset(vk11_props->deviceLUID, 0, VK_LUID_SIZE);
   vk11_props->deviceNodeMask = 0;
   vk11_props->deviceLUIDValid = false;
}

static void
vn_physical_device_init_properties(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   struct {
      /* Vulkan 1.1 */
      VkPhysicalDeviceIDProperties id;
      VkPhysicalDeviceSubgroupProperties subgroup;
      VkPhysicalDevicePointClippingProperties point_clipping;
      VkPhysicalDeviceMultiviewProperties multiview;
      VkPhysicalDeviceProtectedMemoryProperties protected_memory;
      VkPhysicalDeviceMaintenance3Properties maintenance_3;

      /* Vulkan 1.2 */
      VkPhysicalDeviceDriverProperties driver;
      VkPhysicalDeviceFloatControlsProperties float_controls;
      VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing;
      VkPhysicalDeviceDepthStencilResolveProperties depth_stencil_resolve;
      VkPhysicalDeviceSamplerFilterMinmaxProperties sampler_filter_minmax;
      VkPhysicalDeviceTimelineSemaphoreProperties timeline_semaphore;
   } local_props;

   physical_dev->properties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
   if (physical_dev->renderer_version >= VK_API_VERSION_1_2) {
      physical_dev->properties.pNext = &physical_dev->vulkan_1_1_properties;

      physical_dev->vulkan_1_1_properties.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
      physical_dev->vulkan_1_1_properties.pNext =
         &physical_dev->vulkan_1_2_properties;
      physical_dev->vulkan_1_2_properties.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
      physical_dev->vulkan_1_2_properties.pNext = NULL;
   } else {
      physical_dev->properties.pNext = &local_props.id;

      local_props.id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
      local_props.id.pNext = &local_props.subgroup;
      local_props.subgroup.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
      local_props.subgroup.pNext = &local_props.point_clipping;
      local_props.point_clipping.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES;
      local_props.point_clipping.pNext = &local_props.multiview;
      local_props.multiview.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES;
      local_props.multiview.pNext = &local_props.protected_memory;
      local_props.protected_memory.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES;
      local_props.protected_memory.pNext = &local_props.maintenance_3;
      local_props.maintenance_3.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES;
      local_props.maintenance_3.pNext = &local_props.driver;

      local_props.driver.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      local_props.driver.pNext = &local_props.float_controls;
      local_props.float_controls.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
      local_props.float_controls.pNext = &local_props.descriptor_indexing;
      local_props.descriptor_indexing.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
      local_props.descriptor_indexing.pNext =
         &local_props.depth_stencil_resolve;
      local_props.depth_stencil_resolve.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
      local_props.depth_stencil_resolve.pNext =
         &local_props.sampler_filter_minmax;
      local_props.sampler_filter_minmax.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES;
      local_props.sampler_filter_minmax.pNext =
         &local_props.timeline_semaphore;
      local_props.timeline_semaphore.sType =
         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES;
      local_props.timeline_semaphore.pNext = NULL;
   }

   vn_call_vkGetPhysicalDeviceProperties2(
      instance, vn_physical_device_to_handle(physical_dev),
      &physical_dev->properties);

   const struct vk_device_extension_table *exts =
      &physical_dev->renderer_extensions;
   struct VkPhysicalDeviceProperties *props =
      &physical_dev->properties.properties;
   struct VkPhysicalDeviceVulkan11Properties *vk11_props =
      &physical_dev->vulkan_1_1_properties;
   struct VkPhysicalDeviceVulkan12Properties *vk12_props =
      &physical_dev->vulkan_1_2_properties;

   if (physical_dev->renderer_version < VK_API_VERSION_1_2) {
      memcpy(vk11_props->deviceUUID, local_props.id.deviceUUID,
             sizeof(vk11_props->deviceUUID));
      memcpy(vk11_props->driverUUID, local_props.id.driverUUID,
             sizeof(vk11_props->driverUUID));
      memcpy(vk11_props->deviceLUID, local_props.id.deviceLUID,
             sizeof(vk11_props->deviceLUID));
      vk11_props->deviceNodeMask = local_props.id.deviceNodeMask;
      vk11_props->deviceLUIDValid = local_props.id.deviceLUIDValid;

      vk11_props->subgroupSize = local_props.subgroup.subgroupSize;
      vk11_props->subgroupSupportedStages =
         local_props.subgroup.supportedStages;
      vk11_props->subgroupSupportedOperations =
         local_props.subgroup.supportedOperations;
      vk11_props->subgroupQuadOperationsInAllStages =
         local_props.subgroup.quadOperationsInAllStages;

      vk11_props->pointClippingBehavior =
         local_props.point_clipping.pointClippingBehavior;

      vk11_props->maxMultiviewViewCount =
         local_props.multiview.maxMultiviewViewCount;
      vk11_props->maxMultiviewInstanceIndex =
         local_props.multiview.maxMultiviewInstanceIndex;

      vk11_props->protectedNoFault =
         local_props.protected_memory.protectedNoFault;

      vk11_props->maxPerSetDescriptors =
         local_props.maintenance_3.maxPerSetDescriptors;
      vk11_props->maxMemoryAllocationSize =
         local_props.maintenance_3.maxMemoryAllocationSize;

      if (exts->KHR_driver_properties) {
         vk12_props->driverID = local_props.driver.driverID;
         memcpy(vk12_props->driverName, local_props.driver.driverName,
                VK_MAX_DRIVER_NAME_SIZE);
         memcpy(vk12_props->driverInfo, local_props.driver.driverInfo,
                VK_MAX_DRIVER_INFO_SIZE);
         vk12_props->conformanceVersion =
            local_props.driver.conformanceVersion;
      }
      if (exts->KHR_shader_float_controls) {
         vk12_props->denormBehaviorIndependence =
            local_props.float_controls.denormBehaviorIndependence;
         vk12_props->roundingModeIndependence =
            local_props.float_controls.roundingModeIndependence;
         vk12_props->shaderSignedZeroInfNanPreserveFloat16 =
            local_props.float_controls.shaderSignedZeroInfNanPreserveFloat16;
         vk12_props->shaderSignedZeroInfNanPreserveFloat32 =
            local_props.float_controls.shaderSignedZeroInfNanPreserveFloat32;
         vk12_props->shaderSignedZeroInfNanPreserveFloat64 =
            local_props.float_controls.shaderSignedZeroInfNanPreserveFloat64;
         vk12_props->shaderDenormPreserveFloat16 =
            local_props.float_controls.shaderDenormPreserveFloat16;
         vk12_props->shaderDenormPreserveFloat32 =
            local_props.float_controls.shaderDenormPreserveFloat32;
         vk12_props->shaderDenormPreserveFloat64 =
            local_props.float_controls.shaderDenormPreserveFloat64;
         vk12_props->shaderDenormFlushToZeroFloat16 =
            local_props.float_controls.shaderDenormFlushToZeroFloat16;
         vk12_props->shaderDenormFlushToZeroFloat32 =
            local_props.float_controls.shaderDenormFlushToZeroFloat32;
         vk12_props->shaderDenormFlushToZeroFloat64 =
            local_props.float_controls.shaderDenormFlushToZeroFloat64;
         vk12_props->shaderRoundingModeRTEFloat16 =
            local_props.float_controls.shaderRoundingModeRTEFloat16;
         vk12_props->shaderRoundingModeRTEFloat32 =
            local_props.float_controls.shaderRoundingModeRTEFloat32;
         vk12_props->shaderRoundingModeRTEFloat64 =
            local_props.float_controls.shaderRoundingModeRTEFloat64;
         vk12_props->shaderRoundingModeRTZFloat16 =
            local_props.float_controls.shaderRoundingModeRTZFloat16;
         vk12_props->shaderRoundingModeRTZFloat32 =
            local_props.float_controls.shaderRoundingModeRTZFloat32;
         vk12_props->shaderRoundingModeRTZFloat64 =
            local_props.float_controls.shaderRoundingModeRTZFloat64;
      }
      if (exts->EXT_descriptor_indexing) {
         vk12_props->maxUpdateAfterBindDescriptorsInAllPools =
            local_props.descriptor_indexing
               .maxUpdateAfterBindDescriptorsInAllPools;
         vk12_props->shaderUniformBufferArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderUniformBufferArrayNonUniformIndexingNative;
         vk12_props->shaderSampledImageArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderSampledImageArrayNonUniformIndexingNative;
         vk12_props->shaderStorageBufferArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderStorageBufferArrayNonUniformIndexingNative;
         vk12_props->shaderStorageImageArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderStorageImageArrayNonUniformIndexingNative;
         vk12_props->shaderInputAttachmentArrayNonUniformIndexingNative =
            local_props.descriptor_indexing
               .shaderInputAttachmentArrayNonUniformIndexingNative;
         vk12_props->robustBufferAccessUpdateAfterBind =
            local_props.descriptor_indexing.robustBufferAccessUpdateAfterBind;
         vk12_props->quadDivergentImplicitLod =
            local_props.descriptor_indexing.quadDivergentImplicitLod;
         vk12_props->maxPerStageDescriptorUpdateAfterBindSamplers =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindSamplers;
         vk12_props->maxPerStageDescriptorUpdateAfterBindUniformBuffers =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindUniformBuffers;
         vk12_props->maxPerStageDescriptorUpdateAfterBindStorageBuffers =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindStorageBuffers;
         vk12_props->maxPerStageDescriptorUpdateAfterBindSampledImages =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindSampledImages;
         vk12_props->maxPerStageDescriptorUpdateAfterBindStorageImages =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindStorageImages;
         vk12_props->maxPerStageDescriptorUpdateAfterBindInputAttachments =
            local_props.descriptor_indexing
               .maxPerStageDescriptorUpdateAfterBindInputAttachments;
         vk12_props->maxPerStageUpdateAfterBindResources =
            local_props.descriptor_indexing
               .maxPerStageUpdateAfterBindResources;
         vk12_props->maxDescriptorSetUpdateAfterBindSamplers =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindSamplers;
         vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffers =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindUniformBuffers;
         vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
         vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffers =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindStorageBuffers;
         vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
         vk12_props->maxDescriptorSetUpdateAfterBindSampledImages =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindSampledImages;
         vk12_props->maxDescriptorSetUpdateAfterBindStorageImages =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindStorageImages;
         vk12_props->maxDescriptorSetUpdateAfterBindInputAttachments =
            local_props.descriptor_indexing
               .maxDescriptorSetUpdateAfterBindInputAttachments;
      }
      if (exts->KHR_depth_stencil_resolve) {
         vk12_props->supportedDepthResolveModes =
            local_props.depth_stencil_resolve.supportedDepthResolveModes;
         vk12_props->supportedStencilResolveModes =
            local_props.depth_stencil_resolve.supportedStencilResolveModes;
         vk12_props->independentResolveNone =
            local_props.depth_stencil_resolve.independentResolveNone;
         vk12_props->independentResolve =
            local_props.depth_stencil_resolve.independentResolve;
      }
      if (exts->EXT_sampler_filter_minmax) {
         vk12_props->filterMinmaxSingleComponentFormats =
            local_props.sampler_filter_minmax
               .filterMinmaxSingleComponentFormats;
         vk12_props->filterMinmaxImageComponentMapping =
            local_props.sampler_filter_minmax
               .filterMinmaxImageComponentMapping;
      }
      if (exts->KHR_timeline_semaphore) {
         vk12_props->maxTimelineSemaphoreValueDifference =
            local_props.timeline_semaphore.maxTimelineSemaphoreValueDifference;
      }

      vk12_props->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   }

   const uint32_t version_override = vk_get_version_override();
   if (version_override) {
      props->apiVersion = version_override;
   } else {
      if (props->apiVersion > VK_HEADER_VERSION_COMPLETE)
         props->apiVersion = VK_HEADER_VERSION_COMPLETE;
      if (props->apiVersion > vn_info_vk_xml_version())
         props->apiVersion = vn_info_vk_xml_version();
      if (!instance->renderer_info.has_timeline_sync &&
          props->apiVersion >= VK_API_VERSION_1_2)
         props->apiVersion = VK_MAKE_VERSION(1, 1, 130);
   }

   props->driverVersion = vk_get_driver_version();
   props->vendorID = instance->renderer_info.pci.vendor_id;
   props->deviceID = instance->renderer_info.pci.device_id;
   /* some apps don't like VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU */
   props->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
   snprintf(props->deviceName, sizeof(props->deviceName), "Virtio GPU");

   vk12_props->driverID = 0;
   snprintf(vk12_props->driverName, sizeof(vk12_props->driverName), "venus");
   snprintf(vk12_props->driverInfo, sizeof(vk12_props->driverInfo),
            "Mesa " PACKAGE_VERSION MESA_GIT_SHA1);
   vk12_props->conformanceVersion = (VkConformanceVersionKHR){
      .major = 0,
      .minor = 0,
      .subminor = 0,
      .patch = 0,
   };

   vn_physical_device_init_uuids(physical_dev);
}

static VkResult
vn_physical_device_init_queue_family_properties(
   struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;
   uint32_t count;

   vn_call_vkGetPhysicalDeviceQueueFamilyProperties2(
      instance, vn_physical_device_to_handle(physical_dev), &count, NULL);

   uint32_t *sync_queue_bases;
   VkQueueFamilyProperties2 *props =
      vk_alloc(alloc, (sizeof(*props) + sizeof(*sync_queue_bases)) * count,
               VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!props)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   sync_queue_bases = (uint32_t *)&props[count];

   for (uint32_t i = 0; i < count; i++) {
      props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
      /* define an extension to query sync queue base? */
      props[i].pNext = NULL;
   }
   vn_call_vkGetPhysicalDeviceQueueFamilyProperties2(
      instance, vn_physical_device_to_handle(physical_dev), &count, props);

   physical_dev->queue_family_properties = props;
   /* sync_queue_bases will be initialized later */
   physical_dev->queue_family_sync_queue_bases = sync_queue_bases;
   physical_dev->queue_family_count = count;

   return VK_SUCCESS;
}

static void
vn_physical_device_init_memory_properties(
   struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;

   physical_dev->memory_properties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;

   vn_call_vkGetPhysicalDeviceMemoryProperties2(
      instance, vn_physical_device_to_handle(physical_dev),
      &physical_dev->memory_properties);

   if (!instance->renderer_info.has_cache_management) {
      VkPhysicalDeviceMemoryProperties *props =
         &physical_dev->memory_properties.memoryProperties;
      const uint32_t host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

      for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
         const bool coherent = props->memoryTypes[i].propertyFlags &
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
         if (!coherent)
            props->memoryTypes[i].propertyFlags &= ~host_flags;
      }
   }
}

static void
vn_physical_device_init_external_memory_handles(
   struct vn_physical_device *physical_dev)
{
   if (!physical_dev->instance->renderer_info.has_dmabuf_import)
      return;

   /* We have export support but we don't advertise it.  It is for WSI only at
    * the moment.  For import support, we need to be able to serialize
    * vkGetMemoryFdPropertiesKHR and VkImportMemoryFdInfoKHR.  We can
    * serialize fd to bo->res_id, but we probably want to add new
    * commands/structs first (using VK_MESA_venus_protocol).
    *
    * We also create a BO when a vn_device_memory is mappable.  We don't know
    * which handle type the renderer uses.  That seems fine though.
    */
}

static void
vn_physical_device_init_external_fence_handles(
   struct vn_physical_device *physical_dev)
{
   if (!physical_dev->instance->renderer_info.has_external_sync)
      return;

   /* In the current model, a vn_fence can be implemented entirely on top of
    * vn_renderer_sync.  All operations can go through the renderer sync.
    *
    * The current code still creates a host-side VkFence, which can be
    * eliminated.  The renderer also lacks proper external sync (i.e.,
    * drm_syncobj) support and we can only support handle types with copy
    * transference (i.e., sync fds).
    *
    * We are considering creating a vn_renderer_sync from a host-side VkFence
    * instead, similar to how a vn_renderer_bo is created from a host-side
    * VkDeviceMemory.  That will require tons of works on the host side, but
    * should allow us to get rid of ring<->renderer syncs in vkQueueSubmit.
    */
   physical_dev->external_fence_handles =
      VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
}

static void
vn_physical_device_init_external_semaphore_handles(
   struct vn_physical_device *physical_dev)
{
   /* In the current model, it is not possible to support external semaphores.
    * At least an external semaphore cannot be waited on GPU in the host but
    * can only be waited on CPU in the guest.
    *
    * A binary vn_semaphore is implemented solely on top of a host-side binary
    * VkSemaphore.  There is no CPU operation against binary semaphroes and
    * there is no need for vn_renderer_sync.
    *
    * A timeline vn_semaphore is implemented on top of both a host-side
    * timeline VkSemaphore and a vn_renderer_sync.  Whenever a timeline
    * vn_semaphore is updated, we make sure both the host-side timeline
    * VkSemaphore and the vn_renderer_sync are updated.  This allows us to use
    * whichever is more convenient depending on the operations: the host-side
    * timeline VkSemaphore for GPU waits and the vn_renderer_sync for CPU
    * waits/gets.
    *
    * To support external semaphores, we should create a vn_renderer_sync from
    * a host-side VkSemaphore instead, similar to how a vn_renderer_bo is
    * created from a host-side VkDeviceMemory.  The reasons to make a similar
    * move for fences apply to timeline semaphores as well.  Besides, the
    * external handle (drm_syncobj or sync file) needs to carry the necessary
    * information to identify the host-side semaphore.
    */
}

static void
vn_physical_device_get_supported_extensions(
   const struct vn_physical_device *device,
   struct vk_device_extension_table *supported,
   struct vk_device_extension_table *recognized)
{
   memset(supported, 0, sizeof(*supported));
   memset(recognized, 0, sizeof(*recognized));
}

static VkResult
vn_physical_device_init_extensions(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   /* get renderer extensions */
   uint32_t count;
   VkResult result = vn_call_vkEnumerateDeviceExtensionProperties(
      instance, vn_physical_device_to_handle(physical_dev), NULL, &count,
      NULL);
   if (result != VK_SUCCESS)
      return result;

   VkExtensionProperties *exts = NULL;
   if (count) {
      exts = vk_alloc(alloc, sizeof(*exts) * count, VN_DEFAULT_ALIGN,
                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!exts)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      result = vn_call_vkEnumerateDeviceExtensionProperties(
         instance, vn_physical_device_to_handle(physical_dev), NULL, &count,
         exts);
      if (result < VK_SUCCESS) {
         vk_free(alloc, exts);
         return result;
      }
   }

   struct vk_device_extension_table supported;
   struct vk_device_extension_table recognized;
   vn_physical_device_get_supported_extensions(physical_dev, &supported,
                                               &recognized);
   if (!instance->renderer_info.has_timeline_sync)
      recognized.KHR_timeline_semaphore = false;

   physical_dev->extension_spec_versions =
      vk_zalloc(alloc,
                sizeof(*physical_dev->extension_spec_versions) *
                   VK_DEVICE_EXTENSION_COUNT,
                VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!physical_dev->extension_spec_versions) {
      vk_free(alloc, exts);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   for (uint32_t i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
      const VkExtensionProperties *props = &vk_device_extensions[i];
      const VkExtensionProperties *renderer_props = NULL;

      for (uint32_t j = 0; j < count; j++) {
         if (!strcmp(props->extensionName, exts[j].extensionName)) {
            physical_dev->renderer_extensions.extensions[i] = true;
            renderer_props = &exts[j];
            break;
         }
      }

      /* does not depend on renderer (e.g., WSI) */
      if (supported.extensions[i]) {
         physical_dev->base.base.supported_extensions.extensions[i] = true;
         physical_dev->extension_spec_versions[i] = props->specVersion;
         continue;
      }

      /* no driver support */
      if (!recognized.extensions[i])
         continue;

      /* check renderer support */
      if (!renderer_props)
         continue;

      /* check encoder support */
      const uint32_t spec_version =
         vn_info_extension_spec_version(props->extensionName);
      if (!spec_version)
         continue;

      physical_dev->base.base.supported_extensions.extensions[i] = true;
      physical_dev->extension_spec_versions[i] =
         MIN2(renderer_props->specVersion, spec_version);
   }

   vk_free(alloc, exts);

   return VK_SUCCESS;
}

static VkResult
vn_physical_device_init_version(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;

   /*
    * We either check and enable VK_KHR_get_physical_device_properties2, or we
    * must use vkGetPhysicalDeviceProperties to get the device-level version.
    */
   VkPhysicalDeviceProperties props;
   vn_call_vkGetPhysicalDeviceProperties(
      instance, vn_physical_device_to_handle(physical_dev), &props);
   if (props.apiVersion < VN_MIN_RENDERER_VERSION) {
      if (VN_DEBUG(INIT)) {
         vn_log(instance, "unsupported renderer device version %d.%d",
                VK_VERSION_MAJOR(props.apiVersion),
                VK_VERSION_MINOR(props.apiVersion));
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   physical_dev->renderer_version = props.apiVersion;
   if (physical_dev->renderer_version > instance->renderer_version)
      physical_dev->renderer_version = instance->renderer_version;

   return VK_SUCCESS;
}

static VkResult
vn_physical_device_init(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   VkResult result = vn_physical_device_init_version(physical_dev);
   if (result != VK_SUCCESS)
      return result;

   result = vn_physical_device_init_extensions(physical_dev);
   if (result != VK_SUCCESS)
      return result;

   /* TODO query all caps with minimal round trips */
   vn_physical_device_init_features(physical_dev);
   vn_physical_device_init_properties(physical_dev);

   result = vn_physical_device_init_queue_family_properties(physical_dev);
   if (result != VK_SUCCESS)
      goto fail;

   vn_physical_device_init_memory_properties(physical_dev);

   vn_physical_device_init_external_memory_handles(physical_dev);
   vn_physical_device_init_external_fence_handles(physical_dev);
   vn_physical_device_init_external_semaphore_handles(physical_dev);

   return VK_SUCCESS;

fail:
   vk_free(alloc, physical_dev->extension_spec_versions);
   vk_free(alloc, physical_dev->queue_family_properties);
   return result;
}

static void
vn_physical_device_fini(struct vn_physical_device *physical_dev)
{
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;

   vk_free(alloc, physical_dev->extension_spec_versions);
   vk_free(alloc, physical_dev->queue_family_properties);

   vn_physical_device_base_fini(&physical_dev->base);
}

static VkResult
vn_instance_enumerate_physical_devices(struct vn_instance *instance)
{
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;
   struct vn_physical_device *physical_devs = NULL;
   VkResult result;

   mtx_lock(&instance->physical_device_mutex);

   if (instance->physical_devices) {
      result = VK_SUCCESS;
      goto out;
   }

   uint32_t count;
   result = vn_call_vkEnumeratePhysicalDevices(
      instance, vn_instance_to_handle(instance), &count, NULL);
   if (result != VK_SUCCESS || !count)
      goto out;

   physical_devs =
      vk_zalloc(alloc, sizeof(*physical_devs) * count, VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!physical_devs) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   VkPhysicalDevice *handles =
      vk_alloc(alloc, sizeof(*handles) * count, VN_DEFAULT_ALIGN,
               VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!handles) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto out;
   }

   for (uint32_t i = 0; i < count; i++) {
      struct vn_physical_device *physical_dev = &physical_devs[i];

      struct vk_physical_device_dispatch_table dispatch_table;
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &vn_physical_device_entrypoints, true);
      result = vn_physical_device_base_init(
         &physical_dev->base, &instance->base, NULL, &dispatch_table);
      if (result != VK_SUCCESS) {
         count = i;
         goto out;
      }

      physical_dev->instance = instance;

      handles[i] = vn_physical_device_to_handle(physical_dev);
   }

   result = vn_call_vkEnumeratePhysicalDevices(
      instance, vn_instance_to_handle(instance), &count, handles);
   vk_free(alloc, handles);

   if (result != VK_SUCCESS)
      goto out;

   uint32_t sync_queue_base = 0;
   uint32_t i = 0;
   while (i < count) {
      struct vn_physical_device *physical_dev = &physical_devs[i];

      result = vn_physical_device_init(physical_dev);
      if (result == VK_SUCCESS) {
         /* TODO assign sync queues more fairly */
         for (uint32_t j = 0; j < physical_dev->queue_family_count; j++) {
            const VkQueueFamilyProperties *props =
               &physical_dev->queue_family_properties[j].queueFamilyProperties;

            if (sync_queue_base + props->queueCount >
                instance->renderer_info.max_sync_queue_count) {
               if (VN_DEBUG(INIT)) {
                  vn_log(instance, "not enough sync queues (max %d)",
                         instance->renderer_info.max_sync_queue_count);
               }
               result = VK_ERROR_INITIALIZATION_FAILED;
               break;
            }

            physical_dev->queue_family_sync_queue_bases[j] = sync_queue_base;
            sync_queue_base += props->queueCount;
         }
      }

      if (result != VK_SUCCESS) {
         vn_physical_device_base_fini(&physical_devs[i].base);
         memmove(&physical_devs[i], &physical_devs[i + 1],
                 sizeof(*physical_devs) * (count - i - 1));
         count--;
         continue;
      }

      i++;
   }

   if (count) {
      instance->physical_devices = physical_devs;
      instance->physical_device_count = count;
      result = VK_SUCCESS;
   }

out:
   if (result != VK_SUCCESS && physical_devs) {
      for (uint32_t i = 0; i < count; i++)
         vn_physical_device_base_fini(&physical_devs[i].base);
      vk_free(alloc, physical_devs);
   }

   mtx_unlock(&instance->physical_device_mutex);
   return result;
}

/* instance commands */

VkResult
vn_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   *pApiVersion = VK_HEADER_VERSION_COMPLETE;
   return VK_SUCCESS;
}

VkResult
vn_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                        uint32_t *pPropertyCount,
                                        VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vn_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &vn_instance_supported_extensions, pPropertyCount, pProperties);
}

VkResult
vn_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                    VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

VkResult
vn_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkInstance *pInstance)
{
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : vn_default_allocator();
   struct vn_instance *instance;
   VkResult result;

   vn_debug_init();

   instance = vk_zalloc(alloc, sizeof(*instance), VN_DEFAULT_ALIGN,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vn_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &vn_instance_entrypoints, true);
   result = vn_instance_base_init(&instance->base,
                                  &vn_instance_supported_extensions,
                                  &dispatch_table, pCreateInfo, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, instance);
      return vn_error(NULL, result);
   }

   mtx_init(&instance->physical_device_mutex, mtx_plain);

   if (!vn_icd_supports_api_version(
          instance->base.base.app_info.api_version)) {
      result = VK_ERROR_INCOMPATIBLE_DRIVER;
      goto fail;
   }

   if (pCreateInfo->enabledLayerCount) {
      result = VK_ERROR_LAYER_NOT_PRESENT;
      goto fail;
   }

   result = vn_instance_init_renderer(instance);
   if (result != VK_SUCCESS)
      goto fail;

   result = vn_instance_init_ring(instance);
   if (result != VK_SUCCESS)
      goto fail;

   result = vn_instance_init_version(instance);
   if (result != VK_SUCCESS)
      goto fail;

   VkInstanceCreateInfo local_create_info;
   local_create_info = *pCreateInfo;
   local_create_info.ppEnabledExtensionNames = NULL;
   local_create_info.enabledExtensionCount = 0;
   pCreateInfo = &local_create_info;

   /* request at least instance->renderer_version */
   VkApplicationInfo local_app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = instance->renderer_version,
   };
   if (instance->base.base.app_info.api_version <
       instance->renderer_version) {
      if (pCreateInfo->pApplicationInfo) {
         local_app_info = *pCreateInfo->pApplicationInfo;
         local_app_info.apiVersion = instance->renderer_version;
      }
      local_create_info.pApplicationInfo = &local_app_info;
   }

   VkInstance instance_handle = vn_instance_to_handle(instance);
   result =
      vn_call_vkCreateInstance(instance, pCreateInfo, NULL, &instance_handle);
   if (result != VK_SUCCESS)
      goto fail;

   driParseOptionInfo(&instance->available_dri_options, vn_dri_options,
                      ARRAY_SIZE(vn_dri_options));
   driParseConfigFiles(&instance->dri_options,
                       &instance->available_dri_options, 0, "venus", NULL,
                       instance->base.base.app_info.app_name,
                       instance->base.base.app_info.app_version,
                       instance->base.base.app_info.engine_name,
                       instance->base.base.app_info.engine_version);

   *pInstance = instance_handle;

   return VK_SUCCESS;

fail:
   if (instance->reply.bo)
      vn_renderer_bo_unref(instance->reply.bo);

   if (instance->ring.bo) {
      uint32_t destroy_ring_data[4];
      struct vn_cs_encoder local_enc = VN_CS_ENCODER_INITIALIZER(
         destroy_ring_data, sizeof(destroy_ring_data));
      vn_encode_vkDestroyRingMESA(&local_enc, 0, instance->ring.id);
      vn_renderer_submit_simple(instance->renderer, destroy_ring_data,
                                vn_cs_encoder_get_len(&local_enc));

      vn_cs_encoder_fini(&instance->ring.upload);
      vn_renderer_bo_unref(instance->ring.bo);
      vn_ring_fini(&instance->ring.ring);
      mtx_destroy(&instance->ring.mutex);
   }

   if (instance->renderer) {
      mtx_destroy(&instance->roundtrip_mutex);
      vn_renderer_destroy(instance->renderer, alloc);
   }

   mtx_destroy(&instance->physical_device_mutex);

   vn_instance_base_fini(&instance->base);
   vk_free(alloc, instance);

   return vn_error(NULL, result);
}

void
vn_DestroyInstance(VkInstance _instance,
                   const VkAllocationCallbacks *pAllocator)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &instance->base.base.alloc;

   if (!instance)
      return;

   if (instance->physical_devices) {
      for (uint32_t i = 0; i < instance->physical_device_count; i++)
         vn_physical_device_fini(&instance->physical_devices[i]);
      vk_free(alloc, instance->physical_devices);
   }

   vn_call_vkDestroyInstance(instance, _instance, NULL);

   vn_renderer_bo_unref(instance->reply.bo);

   uint32_t destroy_ring_data[4];
   struct vn_cs_encoder local_enc =
      VN_CS_ENCODER_INITIALIZER(destroy_ring_data, sizeof(destroy_ring_data));
   vn_encode_vkDestroyRingMESA(&local_enc, 0, instance->ring.id);
   vn_renderer_submit_simple(instance->renderer, destroy_ring_data,
                             vn_cs_encoder_get_len(&local_enc));

   vn_cs_encoder_fini(&instance->ring.upload);
   vn_ring_fini(&instance->ring.ring);
   mtx_destroy(&instance->ring.mutex);
   vn_renderer_bo_unref(instance->ring.bo);

   mtx_destroy(&instance->roundtrip_mutex);
   vn_renderer_destroy(instance->renderer, alloc);

   mtx_destroy(&instance->physical_device_mutex);

   driDestroyOptionCache(&instance->dri_options);
   driDestroyOptionInfo(&instance->available_dri_options);

   vn_instance_base_fini(&instance->base);
   vk_free(alloc, instance);
}

PFN_vkVoidFunction
vn_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);
   return vk_instance_get_proc_addr(&instance->base.base,
                                    &vn_instance_entrypoints, pName);
}

/* physical device commands */

VkResult
vn_EnumeratePhysicalDevices(VkInstance _instance,
                            uint32_t *pPhysicalDeviceCount,
                            VkPhysicalDevice *pPhysicalDevices)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);

   VkResult result = vn_instance_enumerate_physical_devices(instance);
   if (result != VK_SUCCESS)
      return vn_error(instance, result);

   VK_OUTARRAY_MAKE(out, pPhysicalDevices, pPhysicalDeviceCount);
   for (uint32_t i = 0; i < instance->physical_device_count; i++) {
      vk_outarray_append (&out, physical_dev) {
         *physical_dev =
            vn_physical_device_to_handle(&instance->physical_devices[i]);
      }
   }

   return vk_outarray_status(&out);
}

VkResult
vn_EnumeratePhysicalDeviceGroups(
   VkInstance _instance,
   uint32_t *pPhysicalDeviceGroupCount,
   VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
   struct vn_instance *instance = vn_instance_from_handle(_instance);
   const VkAllocationCallbacks *alloc = &instance->base.base.alloc;
   struct vn_physical_device_base *dummy = NULL;
   VkResult result;

   result = vn_instance_enumerate_physical_devices(instance);
   if (result != VK_SUCCESS)
      return vn_error(instance, result);

   /* make sure VkPhysicalDevice point to objects, as they are considered
    * inputs by the encoder
    */
   if (pPhysicalDeviceGroupProperties) {
      const uint32_t count = *pPhysicalDeviceGroupCount;
      const size_t size = sizeof(*dummy) * VK_MAX_DEVICE_GROUP_SIZE * count;

      dummy = vk_zalloc(alloc, size, VN_DEFAULT_ALIGN,
                        VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!dummy)
         return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

      for (uint32_t i = 0; i < count; i++) {
         VkPhysicalDeviceGroupProperties *props =
            &pPhysicalDeviceGroupProperties[i];

         for (uint32_t j = 0; j < VK_MAX_DEVICE_GROUP_SIZE; j++) {
            struct vn_physical_device_base *obj =
               &dummy[VK_MAX_DEVICE_GROUP_SIZE * i + j];
            obj->base.base.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
            props->physicalDevices[j] = (VkPhysicalDevice)obj;
         }
      }
   }

   result = vn_call_vkEnumeratePhysicalDeviceGroups(
      instance, vn_instance_to_handle(instance), pPhysicalDeviceGroupCount,
      pPhysicalDeviceGroupProperties);
   if (result != VK_SUCCESS) {
      if (dummy)
         vk_free(alloc, dummy);
      return vn_error(instance, result);
   }

   if (pPhysicalDeviceGroupProperties) {
      for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; i++) {
         VkPhysicalDeviceGroupProperties *props =
            &pPhysicalDeviceGroupProperties[i];
         for (uint32_t j = 0; j < props->physicalDeviceCount; j++) {
            const vn_object_id id =
               dummy[VK_MAX_DEVICE_GROUP_SIZE * i + j].id;
            struct vn_physical_device *physical_dev =
               vn_instance_find_physical_device(instance, id);
            props->physicalDevices[j] =
               vn_physical_device_to_handle(physical_dev);
         }
      }
   }

   if (dummy)
      vk_free(alloc, dummy);

   return VK_SUCCESS;
}

void
vn_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                             VkPhysicalDeviceFeatures *pFeatures)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   *pFeatures = physical_dev->features.features;
}

void
vn_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceProperties *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   *pProperties = physical_dev->properties.properties;
}

void
vn_GetPhysicalDeviceQueueFamilyProperties(
   VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties *pQueueFamilyProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);
   for (uint32_t i = 0; i < physical_dev->queue_family_count; i++) {
      vk_outarray_append (&out, props) {
         *props =
            physical_dev->queue_family_properties[i].queueFamilyProperties;
      }
   }
}

void
vn_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   *pMemoryProperties = physical_dev->memory_properties.memoryProperties;
}

void
vn_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                     VkFormat format,
                                     VkFormatProperties *pFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO query all formats during init */
   vn_call_vkGetPhysicalDeviceFormatProperties(
      physical_dev->instance, physicalDevice, format, pFormatProperties);
}

VkResult
vn_GetPhysicalDeviceImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   VkImageTiling tiling,
   VkImageUsageFlags usage,
   VkImageCreateFlags flags,
   VkImageFormatProperties *pImageFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO per-device cache */
   VkResult result = vn_call_vkGetPhysicalDeviceImageFormatProperties(
      physical_dev->instance, physicalDevice, format, type, tiling, usage,
      flags, pImageFormatProperties);

   return vn_result(physical_dev->instance, result);
}

void
vn_GetPhysicalDeviceSparseImageFormatProperties(
   VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkImageType type,
   uint32_t samples,
   VkImageUsageFlags usage,
   VkImageTiling tiling,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO per-device cache */
   vn_call_vkGetPhysicalDeviceSparseImageFormatProperties(
      physical_dev->instance, physicalDevice, format, type, samples, usage,
      tiling, pPropertyCount, pProperties);
}

void
vn_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceFeatures2 *pFeatures)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   const struct VkPhysicalDeviceVulkan11Features *vk11_feats =
      &physical_dev->vulkan_1_1_features;
   const struct VkPhysicalDeviceVulkan12Features *vk12_feats =
      &physical_dev->vulkan_1_2_features;
   union {
      VkBaseOutStructure *pnext;

      /* Vulkan 1.1 */
      VkPhysicalDevice16BitStorageFeatures *sixteen_bit_storage;
      VkPhysicalDeviceMultiviewFeatures *multiview;
      VkPhysicalDeviceVariablePointersFeatures *variable_pointers;
      VkPhysicalDeviceProtectedMemoryFeatures *protected_memory;
      VkPhysicalDeviceSamplerYcbcrConversionFeatures *sampler_ycbcr_conversion;
      VkPhysicalDeviceShaderDrawParametersFeatures *shader_draw_parameters;

      /* Vulkan 1.2 */
      VkPhysicalDevice8BitStorageFeatures *eight_bit_storage;
      VkPhysicalDeviceShaderAtomicInt64Features *shader_atomic_int64;
      VkPhysicalDeviceShaderFloat16Int8Features *shader_float16_int8;
      VkPhysicalDeviceDescriptorIndexingFeatures *descriptor_indexing;
      VkPhysicalDeviceScalarBlockLayoutFeatures *scalar_block_layout;
      VkPhysicalDeviceImagelessFramebufferFeatures *imageless_framebuffer;
      VkPhysicalDeviceUniformBufferStandardLayoutFeatures
         *uniform_buffer_standard_layout;
      VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures
         *shader_subgroup_extended_types;
      VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures
         *separate_depth_stencil_layouts;
      VkPhysicalDeviceHostQueryResetFeatures *host_query_reset;
      VkPhysicalDeviceTimelineSemaphoreFeatures *timeline_semaphore;
      VkPhysicalDeviceBufferDeviceAddressFeatures *buffer_device_address;
      VkPhysicalDeviceVulkanMemoryModelFeatures *vulkan_memory_model;
   } u;

   u.pnext = (VkBaseOutStructure *)pFeatures;
   while (u.pnext) {
      void *saved = u.pnext->pNext;
      switch (u.pnext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
         memcpy(u.pnext, &physical_dev->features,
                sizeof(physical_dev->features));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
         memcpy(u.pnext, vk11_feats, sizeof(*vk11_feats));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
         memcpy(u.pnext, vk12_feats, sizeof(*vk12_feats));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
         u.sixteen_bit_storage->storageBuffer16BitAccess =
            vk11_feats->storageBuffer16BitAccess;
         u.sixteen_bit_storage->uniformAndStorageBuffer16BitAccess =
            vk11_feats->uniformAndStorageBuffer16BitAccess;
         u.sixteen_bit_storage->storagePushConstant16 =
            vk11_feats->storagePushConstant16;
         u.sixteen_bit_storage->storageInputOutput16 =
            vk11_feats->storageInputOutput16;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
         u.multiview->multiview = vk11_feats->multiview;
         u.multiview->multiviewGeometryShader =
            vk11_feats->multiviewGeometryShader;
         u.multiview->multiviewTessellationShader =
            vk11_feats->multiviewTessellationShader;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
         u.variable_pointers->variablePointersStorageBuffer =
            vk11_feats->variablePointersStorageBuffer;
         u.variable_pointers->variablePointers = vk11_feats->variablePointers;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
         u.protected_memory->protectedMemory = vk11_feats->protectedMemory;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
         u.sampler_ycbcr_conversion->samplerYcbcrConversion =
            vk11_feats->samplerYcbcrConversion;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
         u.shader_draw_parameters->shaderDrawParameters =
            vk11_feats->shaderDrawParameters;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
         u.eight_bit_storage->storageBuffer8BitAccess =
            vk12_feats->storageBuffer8BitAccess;
         u.eight_bit_storage->uniformAndStorageBuffer8BitAccess =
            vk12_feats->uniformAndStorageBuffer8BitAccess;
         u.eight_bit_storage->storagePushConstant8 =
            vk12_feats->storagePushConstant8;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
         u.shader_atomic_int64->shaderBufferInt64Atomics =
            vk12_feats->shaderBufferInt64Atomics;
         u.shader_atomic_int64->shaderSharedInt64Atomics =
            vk12_feats->shaderSharedInt64Atomics;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
         u.shader_float16_int8->shaderFloat16 = vk12_feats->shaderFloat16;
         u.shader_float16_int8->shaderInt8 = vk12_feats->shaderInt8;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
         u.descriptor_indexing->shaderInputAttachmentArrayDynamicIndexing =
            vk12_feats->shaderInputAttachmentArrayDynamicIndexing;
         u.descriptor_indexing->shaderUniformTexelBufferArrayDynamicIndexing =
            vk12_feats->shaderUniformTexelBufferArrayDynamicIndexing;
         u.descriptor_indexing->shaderStorageTexelBufferArrayDynamicIndexing =
            vk12_feats->shaderStorageTexelBufferArrayDynamicIndexing;
         u.descriptor_indexing->shaderUniformBufferArrayNonUniformIndexing =
            vk12_feats->shaderUniformBufferArrayNonUniformIndexing;
         u.descriptor_indexing->shaderSampledImageArrayNonUniformIndexing =
            vk12_feats->shaderSampledImageArrayNonUniformIndexing;
         u.descriptor_indexing->shaderStorageBufferArrayNonUniformIndexing =
            vk12_feats->shaderStorageBufferArrayNonUniformIndexing;
         u.descriptor_indexing->shaderStorageImageArrayNonUniformIndexing =
            vk12_feats->shaderStorageImageArrayNonUniformIndexing;
         u.descriptor_indexing->shaderInputAttachmentArrayNonUniformIndexing =
            vk12_feats->shaderInputAttachmentArrayNonUniformIndexing;
         u.descriptor_indexing
            ->shaderUniformTexelBufferArrayNonUniformIndexing =
            vk12_feats->shaderUniformTexelBufferArrayNonUniformIndexing;
         u.descriptor_indexing
            ->shaderStorageTexelBufferArrayNonUniformIndexing =
            vk12_feats->shaderStorageTexelBufferArrayNonUniformIndexing;
         u.descriptor_indexing->descriptorBindingUniformBufferUpdateAfterBind =
            vk12_feats->descriptorBindingUniformBufferUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingSampledImageUpdateAfterBind =
            vk12_feats->descriptorBindingSampledImageUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingStorageImageUpdateAfterBind =
            vk12_feats->descriptorBindingStorageImageUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingStorageBufferUpdateAfterBind =
            vk12_feats->descriptorBindingStorageBufferUpdateAfterBind;
         u.descriptor_indexing
            ->descriptorBindingUniformTexelBufferUpdateAfterBind =
            vk12_feats->descriptorBindingUniformTexelBufferUpdateAfterBind;
         u.descriptor_indexing
            ->descriptorBindingStorageTexelBufferUpdateAfterBind =
            vk12_feats->descriptorBindingStorageTexelBufferUpdateAfterBind;
         u.descriptor_indexing->descriptorBindingUpdateUnusedWhilePending =
            vk12_feats->descriptorBindingUpdateUnusedWhilePending;
         u.descriptor_indexing->descriptorBindingPartiallyBound =
            vk12_feats->descriptorBindingPartiallyBound;
         u.descriptor_indexing->descriptorBindingVariableDescriptorCount =
            vk12_feats->descriptorBindingVariableDescriptorCount;
         u.descriptor_indexing->runtimeDescriptorArray =
            vk12_feats->runtimeDescriptorArray;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
         u.scalar_block_layout->scalarBlockLayout =
            vk12_feats->scalarBlockLayout;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
         u.imageless_framebuffer->imagelessFramebuffer =
            vk12_feats->imagelessFramebuffer;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
         u.uniform_buffer_standard_layout->uniformBufferStandardLayout =
            vk12_feats->uniformBufferStandardLayout;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
         u.shader_subgroup_extended_types->shaderSubgroupExtendedTypes =
            vk12_feats->shaderSubgroupExtendedTypes;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
         u.separate_depth_stencil_layouts->separateDepthStencilLayouts =
            vk12_feats->separateDepthStencilLayouts;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
         u.host_query_reset->hostQueryReset = vk12_feats->hostQueryReset;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
         u.timeline_semaphore->timelineSemaphore =
            vk12_feats->timelineSemaphore;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
         u.buffer_device_address->bufferDeviceAddress =
            vk12_feats->bufferDeviceAddress;
         u.buffer_device_address->bufferDeviceAddressCaptureReplay =
            vk12_feats->bufferDeviceAddressCaptureReplay;
         u.buffer_device_address->bufferDeviceAddressMultiDevice =
            vk12_feats->bufferDeviceAddressMultiDevice;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
         u.vulkan_memory_model->vulkanMemoryModel =
            vk12_feats->vulkanMemoryModel;
         u.vulkan_memory_model->vulkanMemoryModelDeviceScope =
            vk12_feats->vulkanMemoryModelDeviceScope;
         u.vulkan_memory_model->vulkanMemoryModelAvailabilityVisibilityChains =
            vk12_feats->vulkanMemoryModelAvailabilityVisibilityChains;
         break;
      default:
         break;
      }
      u.pnext->pNext = saved;

      u.pnext = u.pnext->pNext;
   }
}

void
vn_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceProperties2 *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   const struct VkPhysicalDeviceVulkan11Properties *vk11_props =
      &physical_dev->vulkan_1_1_properties;
   const struct VkPhysicalDeviceVulkan12Properties *vk12_props =
      &physical_dev->vulkan_1_2_properties;
   union {
      VkBaseOutStructure *pnext;

      /* Vulkan 1.1 */
      VkPhysicalDeviceIDProperties *id;
      VkPhysicalDeviceSubgroupProperties *subgroup;
      VkPhysicalDevicePointClippingProperties *point_clipping;
      VkPhysicalDeviceMultiviewProperties *multiview;
      VkPhysicalDeviceProtectedMemoryProperties *protected_memory;
      VkPhysicalDeviceMaintenance3Properties *maintenance_3;

      /* Vulkan 1.2 */
      VkPhysicalDeviceDriverProperties *driver;
      VkPhysicalDeviceFloatControlsProperties *float_controls;
      VkPhysicalDeviceDescriptorIndexingProperties *descriptor_indexing;
      VkPhysicalDeviceDepthStencilResolveProperties *depth_stencil_resolve;
      VkPhysicalDeviceSamplerFilterMinmaxProperties *sampler_filter_minmax;
      VkPhysicalDeviceTimelineSemaphoreProperties *timeline_semaphore;

      VkPhysicalDevicePCIBusInfoPropertiesEXT *pci_bus_info;
   } u;

   u.pnext = (VkBaseOutStructure *)pProperties;
   while (u.pnext) {
      void *saved = u.pnext->pNext;
      switch (u.pnext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2:
         memcpy(u.pnext, &physical_dev->properties,
                sizeof(physical_dev->properties));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
         memcpy(u.pnext, vk11_props, sizeof(*vk11_props));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
         memcpy(u.pnext, vk12_props, sizeof(*vk12_props));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:
         memcpy(u.id->deviceUUID, vk11_props->deviceUUID,
                sizeof(vk11_props->deviceUUID));
         memcpy(u.id->driverUUID, vk11_props->driverUUID,
                sizeof(vk11_props->driverUUID));
         memcpy(u.id->deviceLUID, vk11_props->deviceLUID,
                sizeof(vk11_props->deviceLUID));
         u.id->deviceNodeMask = vk11_props->deviceNodeMask;
         u.id->deviceLUIDValid = vk11_props->deviceLUIDValid;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
         u.subgroup->subgroupSize = vk11_props->subgroupSize;
         u.subgroup->supportedStages = vk11_props->subgroupSupportedStages;
         u.subgroup->supportedOperations =
            vk11_props->subgroupSupportedOperations;
         u.subgroup->quadOperationsInAllStages =
            vk11_props->subgroupQuadOperationsInAllStages;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
         u.point_clipping->pointClippingBehavior =
            vk11_props->pointClippingBehavior;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES:
         u.multiview->maxMultiviewViewCount =
            vk11_props->maxMultiviewViewCount;
         u.multiview->maxMultiviewInstanceIndex =
            vk11_props->maxMultiviewInstanceIndex;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
         u.protected_memory->protectedNoFault = vk11_props->protectedNoFault;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:
         u.maintenance_3->maxPerSetDescriptors =
            vk11_props->maxPerSetDescriptors;
         u.maintenance_3->maxMemoryAllocationSize =
            vk11_props->maxMemoryAllocationSize;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
         u.driver->driverID = vk12_props->driverID;
         memcpy(u.driver->driverName, vk12_props->driverName,
                sizeof(vk12_props->driverName));
         memcpy(u.driver->driverInfo, vk12_props->driverInfo,
                sizeof(vk12_props->driverInfo));
         u.driver->conformanceVersion = vk12_props->conformanceVersion;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES:
         u.float_controls->denormBehaviorIndependence =
            vk12_props->denormBehaviorIndependence;
         u.float_controls->roundingModeIndependence =
            vk12_props->roundingModeIndependence;
         u.float_controls->shaderSignedZeroInfNanPreserveFloat16 =
            vk12_props->shaderSignedZeroInfNanPreserveFloat16;
         u.float_controls->shaderSignedZeroInfNanPreserveFloat32 =
            vk12_props->shaderSignedZeroInfNanPreserveFloat32;
         u.float_controls->shaderSignedZeroInfNanPreserveFloat64 =
            vk12_props->shaderSignedZeroInfNanPreserveFloat64;
         u.float_controls->shaderDenormPreserveFloat16 =
            vk12_props->shaderDenormPreserveFloat16;
         u.float_controls->shaderDenormPreserveFloat32 =
            vk12_props->shaderDenormPreserveFloat32;
         u.float_controls->shaderDenormPreserveFloat64 =
            vk12_props->shaderDenormPreserveFloat64;
         u.float_controls->shaderDenormFlushToZeroFloat16 =
            vk12_props->shaderDenormFlushToZeroFloat16;
         u.float_controls->shaderDenormFlushToZeroFloat32 =
            vk12_props->shaderDenormFlushToZeroFloat32;
         u.float_controls->shaderDenormFlushToZeroFloat64 =
            vk12_props->shaderDenormFlushToZeroFloat64;
         u.float_controls->shaderRoundingModeRTEFloat16 =
            vk12_props->shaderRoundingModeRTEFloat16;
         u.float_controls->shaderRoundingModeRTEFloat32 =
            vk12_props->shaderRoundingModeRTEFloat32;
         u.float_controls->shaderRoundingModeRTEFloat64 =
            vk12_props->shaderRoundingModeRTEFloat64;
         u.float_controls->shaderRoundingModeRTZFloat16 =
            vk12_props->shaderRoundingModeRTZFloat16;
         u.float_controls->shaderRoundingModeRTZFloat32 =
            vk12_props->shaderRoundingModeRTZFloat32;
         u.float_controls->shaderRoundingModeRTZFloat64 =
            vk12_props->shaderRoundingModeRTZFloat64;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
         u.descriptor_indexing->maxUpdateAfterBindDescriptorsInAllPools =
            vk12_props->maxUpdateAfterBindDescriptorsInAllPools;
         u.descriptor_indexing
            ->shaderUniformBufferArrayNonUniformIndexingNative =
            vk12_props->shaderUniformBufferArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderSampledImageArrayNonUniformIndexingNative =
            vk12_props->shaderSampledImageArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderStorageBufferArrayNonUniformIndexingNative =
            vk12_props->shaderStorageBufferArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderStorageImageArrayNonUniformIndexingNative =
            vk12_props->shaderStorageImageArrayNonUniformIndexingNative;
         u.descriptor_indexing
            ->shaderInputAttachmentArrayNonUniformIndexingNative =
            vk12_props->shaderInputAttachmentArrayNonUniformIndexingNative;
         u.descriptor_indexing->robustBufferAccessUpdateAfterBind =
            vk12_props->robustBufferAccessUpdateAfterBind;
         u.descriptor_indexing->quadDivergentImplicitLod =
            vk12_props->quadDivergentImplicitLod;
         u.descriptor_indexing->maxPerStageDescriptorUpdateAfterBindSamplers =
            vk12_props->maxPerStageDescriptorUpdateAfterBindSamplers;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindUniformBuffers =
            vk12_props->maxPerStageDescriptorUpdateAfterBindUniformBuffers;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindStorageBuffers =
            vk12_props->maxPerStageDescriptorUpdateAfterBindStorageBuffers;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindSampledImages =
            vk12_props->maxPerStageDescriptorUpdateAfterBindSampledImages;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindStorageImages =
            vk12_props->maxPerStageDescriptorUpdateAfterBindStorageImages;
         u.descriptor_indexing
            ->maxPerStageDescriptorUpdateAfterBindInputAttachments =
            vk12_props->maxPerStageDescriptorUpdateAfterBindInputAttachments;
         u.descriptor_indexing->maxPerStageUpdateAfterBindResources =
            vk12_props->maxPerStageUpdateAfterBindResources;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindSamplers =
            vk12_props->maxDescriptorSetUpdateAfterBindSamplers;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindUniformBuffers =
            vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffers;
         u.descriptor_indexing
            ->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic =
            vk12_props->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindStorageBuffers =
            vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffers;
         u.descriptor_indexing
            ->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic =
            vk12_props->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindSampledImages =
            vk12_props->maxDescriptorSetUpdateAfterBindSampledImages;
         u.descriptor_indexing->maxDescriptorSetUpdateAfterBindStorageImages =
            vk12_props->maxDescriptorSetUpdateAfterBindStorageImages;
         u.descriptor_indexing
            ->maxDescriptorSetUpdateAfterBindInputAttachments =
            vk12_props->maxDescriptorSetUpdateAfterBindInputAttachments;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:
         u.depth_stencil_resolve->supportedDepthResolveModes =
            vk12_props->supportedDepthResolveModes;
         u.depth_stencil_resolve->supportedStencilResolveModes =
            vk12_props->supportedStencilResolveModes;
         u.depth_stencil_resolve->independentResolveNone =
            vk12_props->independentResolveNone;
         u.depth_stencil_resolve->independentResolve =
            vk12_props->independentResolve;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES:
         u.sampler_filter_minmax->filterMinmaxSingleComponentFormats =
            vk12_props->filterMinmaxSingleComponentFormats;
         u.sampler_filter_minmax->filterMinmaxImageComponentMapping =
            vk12_props->filterMinmaxImageComponentMapping;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
         u.timeline_semaphore->maxTimelineSemaphoreValueDifference =
            vk12_props->maxTimelineSemaphoreValueDifference;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT:
         /* this is used by WSI */
         if (physical_dev->instance->renderer_info.pci.has_bus_info) {
            u.pci_bus_info->pciDomain =
               physical_dev->instance->renderer_info.pci.domain;
            u.pci_bus_info->pciBus =
               physical_dev->instance->renderer_info.pci.bus;
            u.pci_bus_info->pciDevice =
               physical_dev->instance->renderer_info.pci.device;
            u.pci_bus_info->pciFunction =
               physical_dev->instance->renderer_info.pci.function;
         }
         break;
      default:
         break;
      }
      u.pnext->pNext = saved;

      u.pnext = u.pnext->pNext;
   }
}

void
vn_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);
   for (uint32_t i = 0; i < physical_dev->queue_family_count; i++) {
      vk_outarray_append (&out, props) {
         *props = physical_dev->queue_family_properties[i];
      }
   }
}

void
vn_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   pMemoryProperties->memoryProperties =
      physical_dev->memory_properties.memoryProperties;
}

void
vn_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                      VkFormat format,
                                      VkFormatProperties2 *pFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO query all formats during init */
   vn_call_vkGetPhysicalDeviceFormatProperties2(
      physical_dev->instance, physicalDevice, format, pFormatProperties);
}

VkResult
vn_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkImageFormatProperties2 *pImageFormatProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   const VkPhysicalDeviceExternalImageFormatInfo *external_info =
      vk_find_struct_const(pImageFormatInfo->pNext,
                           PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   if (external_info && !external_info->handleType)
      external_info = NULL;

   if (external_info &&
       !(external_info->handleType & physical_dev->external_memory_handles))
      return vn_error(physical_dev->instance, VK_ERROR_FORMAT_NOT_SUPPORTED);

   VkResult result;
   /* TODO per-device cache */
   result = vn_call_vkGetPhysicalDeviceImageFormatProperties2(
      physical_dev->instance, physicalDevice, pImageFormatInfo,
      pImageFormatProperties);

   if (result == VK_SUCCESS && external_info) {
      VkExternalImageFormatProperties *img_props = vk_find_struct(
         pImageFormatProperties->pNext, EXTERNAL_IMAGE_FORMAT_PROPERTIES);
      VkExternalMemoryProperties *mem_props =
         &img_props->externalMemoryProperties;

      mem_props->compatibleHandleTypes &=
         physical_dev->external_memory_handles;
      mem_props->exportFromImportedHandleTypes &=
         physical_dev->external_memory_handles;
   }

   return vn_result(physical_dev->instance, result);
}

void
vn_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties2 *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   /* TODO per-device cache */
   vn_call_vkGetPhysicalDeviceSparseImageFormatProperties2(
      physical_dev->instance, physicalDevice, pFormatInfo, pPropertyCount,
      pProperties);
}

void
vn_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   VkExternalMemoryProperties *props =
      &pExternalBufferProperties->externalMemoryProperties;

   if (!(pExternalBufferInfo->handleType &
         physical_dev->external_memory_handles)) {
      props->compatibleHandleTypes = pExternalBufferInfo->handleType;
      props->exportFromImportedHandleTypes = 0;
      props->externalMemoryFeatures = 0;
      return;
   }

   /* TODO per-device cache */
   vn_call_vkGetPhysicalDeviceExternalBufferProperties(
      physical_dev->instance, physicalDevice, pExternalBufferInfo,
      pExternalBufferProperties);

   props->compatibleHandleTypes &= physical_dev->external_memory_handles;
   props->exportFromImportedHandleTypes &=
      physical_dev->external_memory_handles;
}

void
vn_GetPhysicalDeviceExternalFenceProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
   VkExternalFenceProperties *pExternalFenceProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   if (pExternalFenceInfo->handleType &
       physical_dev->external_fence_handles) {
      pExternalFenceProperties->compatibleHandleTypes =
         physical_dev->external_fence_handles;
      pExternalFenceProperties->exportFromImportedHandleTypes =
         physical_dev->external_fence_handles;
      pExternalFenceProperties->externalFenceFeatures =
         VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalFenceProperties->compatibleHandleTypes =
         pExternalFenceInfo->handleType;
      pExternalFenceProperties->exportFromImportedHandleTypes = 0;
      pExternalFenceProperties->externalFenceFeatures = 0;
   }
}

void
vn_GetPhysicalDeviceExternalSemaphoreProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
   VkExternalSemaphoreProperties *pExternalSemaphoreProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   const VkSemaphoreTypeCreateInfoKHR *type_info = vk_find_struct_const(
      pExternalSemaphoreInfo->pNext, SEMAPHORE_TYPE_CREATE_INFO_KHR);
   const VkSemaphoreType sem_type =
      type_info ? type_info->semaphoreType : VK_SEMAPHORE_TYPE_BINARY;
   const VkExternalSemaphoreHandleTypeFlags valid_handles =
      sem_type == VK_SEMAPHORE_TYPE_BINARY
         ? physical_dev->external_binary_semaphore_handles
         : physical_dev->external_timeline_semaphore_handles;
   if (pExternalSemaphoreInfo->handleType & valid_handles) {
      pExternalSemaphoreProperties->compatibleHandleTypes = valid_handles;
      pExternalSemaphoreProperties->exportFromImportedHandleTypes =
         valid_handles;
      pExternalSemaphoreProperties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalSemaphoreProperties->compatibleHandleTypes =
         pExternalSemaphoreInfo->handleType;
      pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
      pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
   }
}

/* device commands */

VkResult
vn_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                      const char *pLayerName,
                                      uint32_t *pPropertyCount,
                                      VkExtensionProperties *pProperties)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);

   if (pLayerName)
      return vn_error(physical_dev->instance, VK_ERROR_LAYER_NOT_PRESENT);

   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);
   for (uint32_t i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
      if (physical_dev->base.base.supported_extensions.extensions[i]) {
         vk_outarray_append (&out, prop) {
            *prop = vk_device_extensions[i];
            prop->specVersion = physical_dev->extension_spec_versions[i];
         }
      }
   }

   return vk_outarray_status(&out);
}

VkResult
vn_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                  uint32_t *pPropertyCount,
                                  VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

static VkResult
vn_queue_init(struct vn_device *dev,
              struct vn_queue *queue,
              const VkDeviceQueueCreateInfo *queue_info,
              uint32_t queue_index,
              uint32_t sync_queue_index)
{
   vn_object_base_init(&queue->base, VK_OBJECT_TYPE_QUEUE, &dev->base);

   VkQueue queue_handle = vn_queue_to_handle(queue);
   vn_async_vkGetDeviceQueue2(
      dev->instance, vn_device_to_handle(dev),
      &(VkDeviceQueueInfo2){
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
         .flags = queue_info->flags,
         .queueFamilyIndex = queue_info->queueFamilyIndex,
         .queueIndex = queue_index,
      },
      &queue_handle);

   queue->device = dev;
   queue->family = queue_info->queueFamilyIndex;
   queue->index = queue_index;
   queue->flags = queue_info->flags;

   queue->sync_queue_index = sync_queue_index;

   VkResult result =
      vn_renderer_sync_create_cpu(dev->instance->renderer, &queue->idle_sync);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

static VkResult
vn_device_init_queues(struct vn_device *dev,
                      const VkDeviceCreateInfo *create_info)
{
   struct vn_physical_device *physical_dev = dev->physical_device;
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   uint32_t count = 0;
   for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++)
      count += create_info->pQueueCreateInfos[i].queueCount;

   struct vn_queue *queues =
      vk_zalloc(alloc, sizeof(*queues) * count, VN_DEFAULT_ALIGN,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!queues)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult result = VK_SUCCESS;
   count = 0;
   for (uint32_t i = 0; i < create_info->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_info =
         &create_info->pQueueCreateInfos[i];
      const uint32_t sync_queue_base =
         physical_dev
            ->queue_family_sync_queue_bases[queue_info->queueFamilyIndex];

      for (uint32_t j = 0; j < queue_info->queueCount; j++) {
         result = vn_queue_init(dev, &queues[count], queue_info, j,
                                sync_queue_base + j);
         if (result != VK_SUCCESS)
            break;

         count++;
      }
   }

   if (result != VK_SUCCESS) {
      for (uint32_t i = 0; i < count; i++)
         vn_renderer_sync_destroy(queues[i].idle_sync);
      vk_free(alloc, queues);

      return result;
   }

   dev->queues = queues;
   dev->queue_count = count;

   return VK_SUCCESS;
}

VkResult
vn_CreateDevice(VkPhysicalDevice physicalDevice,
                const VkDeviceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkDevice *pDevice)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   struct vn_instance *instance = physical_dev->instance;
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &instance->base.base.alloc;
   struct vn_device *dev;
   VkResult result;

   dev = vk_zalloc(alloc, sizeof(*dev), VN_DEFAULT_ALIGN,
                   VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!dev)
      return vn_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &vn_device_entrypoints, true);
   result = vn_device_base_init(&dev->base, &physical_dev->base,
                                &dispatch_table, pCreateInfo, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, dev);
      return vn_error(instance, result);
   }

   dev->instance = instance;
   dev->physical_device = physical_dev;

   VkDevice dev_handle = vn_device_to_handle(dev);
   result = vn_call_vkCreateDevice(instance, physicalDevice, pCreateInfo,
                                   NULL, &dev_handle);
   if (result != VK_SUCCESS)
      goto fail;

   result = vn_device_init_queues(dev, pCreateInfo);
   if (result != VK_SUCCESS) {
      vn_call_vkDestroyDevice(instance, dev_handle, NULL);
      goto fail;
   }

   *pDevice = dev_handle;

   return VK_SUCCESS;

fail:
   vn_device_base_fini(&dev->base);
   vk_free(alloc, dev);
   return vn_error(instance, result);
}

void
vn_DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!dev)
      return;

   vn_async_vkDestroyDevice(dev->instance, device, NULL);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      vn_renderer_sync_destroy(queue->idle_sync);
      vn_object_base_fini(&queue->base);
   }
   vk_free(alloc, dev->queues);

   vn_device_base_fini(&dev->base);
   vk_free(alloc, dev);
}

PFN_vkVoidFunction
vn_GetDeviceProcAddr(VkDevice device, const char *pName)
{
   struct vn_device *dev = vn_device_from_handle(device);
   return vk_device_get_proc_addr(&dev->base.base, pName);
}

void
vn_GetDeviceGroupPeerMemoryFeatures(
   VkDevice device,
   uint32_t heapIndex,
   uint32_t localDeviceIndex,
   uint32_t remoteDeviceIndex,
   VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO get and cache the values in vkCreateDevice */
   vn_call_vkGetDeviceGroupPeerMemoryFeatures(
      dev->instance, device, heapIndex, localDeviceIndex, remoteDeviceIndex,
      pPeerMemoryFeatures);
}

VkResult
vn_DeviceWaitIdle(VkDevice device)
{
   struct vn_device *dev = vn_device_from_handle(device);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      VkResult result = vn_QueueWaitIdle(vn_queue_to_handle(queue));
      if (result != VK_SUCCESS)
         return vn_error(dev->instance, result);
   }

   return VK_SUCCESS;
}

/* queue commands */

void
vn_GetDeviceQueue(VkDevice device,
                  uint32_t queueFamilyIndex,
                  uint32_t queueIndex,
                  VkQueue *pQueue)
{
   struct vn_device *dev = vn_device_from_handle(device);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      if (queue->family == queueFamilyIndex && queue->index == queueIndex) {
         assert(!queue->flags);
         *pQueue = vn_queue_to_handle(queue);
         return;
      }
   }
   unreachable("bad queue family/index");
}

void
vn_GetDeviceQueue2(VkDevice device,
                   const VkDeviceQueueInfo2 *pQueueInfo,
                   VkQueue *pQueue)
{
   struct vn_device *dev = vn_device_from_handle(device);

   for (uint32_t i = 0; i < dev->queue_count; i++) {
      struct vn_queue *queue = &dev->queues[i];
      if (queue->family == pQueueInfo->queueFamilyIndex &&
          queue->index == pQueueInfo->queueIndex &&
          queue->flags == pQueueInfo->flags) {
         *pQueue = vn_queue_to_handle(queue);
         return;
      }
   }
   unreachable("bad queue family/index");
}

static void
vn_semaphore_reset_wsi(struct vn_device *dev, struct vn_semaphore *sem);

struct vn_queue_submission {
   VkStructureType batch_type;
   VkQueue queue;
   uint32_t batch_count;
   union {
      const void *batches;
      const VkSubmitInfo *submit_batches;
      const VkBindSparseInfo *bind_sparse_batches;
   };
   VkFence fence;

   uint32_t wait_semaphore_count;
   uint32_t wait_wsi_count;
   uint32_t signal_semaphore_count;
   uint32_t signal_device_only_count;
   uint32_t signal_timeline_count;

   uint32_t sync_count;

   struct {
      void *storage;

      union {
         void *batches;
         VkSubmitInfo *submit_batches;
         VkBindSparseInfo *bind_sparse_batches;
      };
      VkSemaphore *semaphores;

      struct vn_renderer_sync **syncs;
      uint64_t *sync_values;

      uint32_t *batch_sync_counts;
   } temp;
};

static void
vn_queue_submission_count_semaphores(struct vn_queue_submission *submit)
{
   submit->wait_semaphore_count = 0;
   submit->wait_wsi_count = 0;
   submit->signal_semaphore_count = 0;
   submit->signal_device_only_count = 0;
   submit->signal_timeline_count = 0;
   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      for (uint32_t i = 0; i < submit->batch_count; i++) {
         const VkSubmitInfo *batch = &submit->submit_batches[i];

         submit->wait_semaphore_count += batch->waitSemaphoreCount;
         submit->signal_semaphore_count += batch->signalSemaphoreCount;

         for (uint32_t j = 0; j < batch->waitSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pWaitSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
               submit->wait_wsi_count++;
         }

         for (uint32_t j = 0; j < batch->signalSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pSignalSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            /* it must be one of the waited semaphores and will be reset */
            if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
               payload = &sem->permanent;

            if (payload->type == VN_SYNC_TYPE_DEVICE_ONLY)
               submit->signal_device_only_count++;
            else if (sem->type == VK_SEMAPHORE_TYPE_TIMELINE)
               submit->signal_timeline_count++;
         }
      }
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      for (uint32_t i = 0; i < submit->batch_count; i++) {
         const VkBindSparseInfo *batch = &submit->bind_sparse_batches[i];

         submit->wait_semaphore_count += batch->waitSemaphoreCount;
         submit->signal_semaphore_count += batch->signalSemaphoreCount;

         for (uint32_t j = 0; j < batch->waitSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pWaitSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
               submit->wait_wsi_count++;
         }

         for (uint32_t j = 0; j < batch->signalSemaphoreCount; j++) {
            struct vn_semaphore *sem =
               vn_semaphore_from_handle(batch->pSignalSemaphores[j]);
            const struct vn_sync_payload *payload = sem->payload;

            if (payload->type == VN_SYNC_TYPE_DEVICE_ONLY)
               submit->signal_device_only_count++;
            else if (sem->type == VK_SEMAPHORE_TYPE_TIMELINE)
               submit->signal_timeline_count++;
         }
      }
      break;
   default:
      unreachable("unexpected batch type");
      break;
   }

   submit->sync_count =
      submit->signal_semaphore_count - submit->signal_device_only_count;
   if (submit->fence != VK_NULL_HANDLE)
      submit->sync_count++;
}

static VkResult
vn_queue_submission_alloc_storage(struct vn_queue_submission *submit)
{
   struct vn_queue *queue = vn_queue_from_handle(submit->queue);
   const VkAllocationCallbacks *alloc = &queue->device->base.base.alloc;
   size_t alloc_size = 0;
   size_t semaphores_offset = 0;
   size_t syncs_offset = 0;
   size_t sync_values_offset = 0;
   size_t batch_sync_counts_offset = 0;

   /* we want to filter out VN_SYNC_TYPE_WSI_SIGNALED wait semaphores */
   if (submit->wait_wsi_count) {
      switch (submit->batch_type) {
      case VK_STRUCTURE_TYPE_SUBMIT_INFO:
         alloc_size += sizeof(VkSubmitInfo) * submit->batch_count;
         break;
      case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
         alloc_size += sizeof(VkBindSparseInfo) * submit->batch_count;
         break;
      default:
         unreachable("unexpected batch type");
         break;
      }

      semaphores_offset = alloc_size;
      alloc_size += sizeof(*submit->temp.semaphores) *
                    (submit->wait_semaphore_count - submit->wait_wsi_count);
   }

   if (submit->sync_count) {
      syncs_offset = alloc_size;
      alloc_size += sizeof(*submit->temp.syncs) * submit->sync_count;

      alloc_size = (alloc_size + 7) & ~7;
      sync_values_offset = alloc_size;
      alloc_size += sizeof(*submit->temp.sync_values) * submit->sync_count;

      batch_sync_counts_offset = alloc_size;
      alloc_size +=
         sizeof(*submit->temp.batch_sync_counts) * submit->batch_count;
   }

   if (!alloc_size) {
      submit->temp.storage = NULL;
      return VK_SUCCESS;
   }

   submit->temp.storage = vk_alloc(alloc, alloc_size, VN_DEFAULT_ALIGN,
                                   VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!submit->temp.storage)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   submit->temp.batches = submit->temp.storage;
   submit->temp.semaphores = submit->temp.storage + semaphores_offset;

   submit->temp.syncs = submit->temp.storage + syncs_offset;
   submit->temp.sync_values = submit->temp.storage + sync_values_offset;
   submit->temp.batch_sync_counts =
      submit->temp.storage + batch_sync_counts_offset;

   return VK_SUCCESS;
}

static uint32_t
vn_queue_submission_filter_batch_wsi_semaphores(
   struct vn_queue_submission *submit,
   uint32_t batch_index,
   uint32_t sem_base)
{
   struct vn_queue *queue = vn_queue_from_handle(submit->queue);

   union {
      VkSubmitInfo *submit_batch;
      VkBindSparseInfo *bind_sparse_batch;
   } u;
   const VkSemaphore *src_sems;
   uint32_t src_count;
   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      u.submit_batch = &submit->temp.submit_batches[batch_index];
      src_sems = u.submit_batch->pWaitSemaphores;
      src_count = u.submit_batch->waitSemaphoreCount;
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      u.bind_sparse_batch = &submit->temp.bind_sparse_batches[batch_index];
      src_sems = u.bind_sparse_batch->pWaitSemaphores;
      src_count = u.bind_sparse_batch->waitSemaphoreCount;
      break;
   default:
      unreachable("unexpected batch type");
      break;
   }

   VkSemaphore *dst_sems = &submit->temp.semaphores[sem_base];
   uint32_t dst_count = 0;

   /* filter out VN_SYNC_TYPE_WSI_SIGNALED wait semaphores */
   for (uint32_t i = 0; i < src_count; i++) {
      struct vn_semaphore *sem = vn_semaphore_from_handle(src_sems[i]);
      const struct vn_sync_payload *payload = sem->payload;

      if (payload->type == VN_SYNC_TYPE_WSI_SIGNALED)
         vn_semaphore_reset_wsi(queue->device, sem);
      else
         dst_sems[dst_count++] = src_sems[i];
   }

   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      u.submit_batch->pWaitSemaphores = dst_sems;
      u.submit_batch->waitSemaphoreCount = dst_count;
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      u.bind_sparse_batch->pWaitSemaphores = dst_sems;
      u.bind_sparse_batch->waitSemaphoreCount = dst_count;
      break;
   default:
      break;
   }

   return dst_count;
}

static uint32_t
vn_queue_submission_setup_batch_syncs(struct vn_queue_submission *submit,
                                      uint32_t batch_index,
                                      uint32_t sync_base)
{
   union {
      const VkSubmitInfo *submit_batch;
      const VkBindSparseInfo *bind_sparse_batch;
   } u;
   const VkTimelineSemaphoreSubmitInfo *timeline;
   const VkSemaphore *sems;
   uint32_t sem_count;
   switch (submit->batch_type) {
   case VK_STRUCTURE_TYPE_SUBMIT_INFO:
      u.submit_batch = &submit->submit_batches[batch_index];
      timeline = vk_find_struct_const(u.submit_batch->pNext,
                                      TIMELINE_SEMAPHORE_SUBMIT_INFO);
      sems = u.submit_batch->pSignalSemaphores;
      sem_count = u.submit_batch->signalSemaphoreCount;
      break;
   case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
      u.bind_sparse_batch = &submit->bind_sparse_batches[batch_index];
      timeline = vk_find_struct_const(u.bind_sparse_batch->pNext,
                                      TIMELINE_SEMAPHORE_SUBMIT_INFO);
      sems = u.bind_sparse_batch->pSignalSemaphores;
      sem_count = u.bind_sparse_batch->signalSemaphoreCount;
      break;
   default:
      unreachable("unexpected batch type");
      break;
   }

   struct vn_renderer_sync **syncs = &submit->temp.syncs[sync_base];
   uint64_t *sync_values = &submit->temp.sync_values[sync_base];
   uint32_t sync_count = 0;

   for (uint32_t i = 0; i < sem_count; i++) {
      struct vn_semaphore *sem = vn_semaphore_from_handle(sems[i]);
      const struct vn_sync_payload *payload = sem->payload;

      if (payload->type == VN_SYNC_TYPE_DEVICE_ONLY)
         continue;

      assert(payload->type == VN_SYNC_TYPE_SYNC);
      syncs[sync_count] = payload->sync;
      sync_values[sync_count] = sem->type == VK_SEMAPHORE_TYPE_TIMELINE
                                   ? timeline->pSignalSemaphoreValues[i]
                                   : 1;
      sync_count++;
   }

   submit->temp.batch_sync_counts[batch_index] = sync_count;

   return sync_count;
}

static uint32_t
vn_queue_submission_setup_fence_sync(struct vn_queue_submission *submit,
                                     uint32_t sync_base)
{
   if (submit->fence == VK_NULL_HANDLE)
      return 0;

   struct vn_fence *fence = vn_fence_from_handle(submit->fence);
   struct vn_sync_payload *payload = fence->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   submit->temp.syncs[sync_base] = payload->sync;
   submit->temp.sync_values[sync_base] = 1;

   return 1;
}

static void
vn_queue_submission_setup_batches(struct vn_queue_submission *submit)
{
   if (!submit->temp.storage)
      return;

   /* make a copy because we need to filter out WSI semaphores */
   if (submit->wait_wsi_count) {
      switch (submit->batch_type) {
      case VK_STRUCTURE_TYPE_SUBMIT_INFO:
         memcpy(submit->temp.submit_batches, submit->submit_batches,
                sizeof(submit->submit_batches[0]) * submit->batch_count);
         submit->submit_batches = submit->temp.submit_batches;
         break;
      case VK_STRUCTURE_TYPE_BIND_SPARSE_INFO:
         memcpy(submit->temp.bind_sparse_batches, submit->bind_sparse_batches,
                sizeof(submit->bind_sparse_batches[0]) * submit->batch_count);
         submit->bind_sparse_batches = submit->temp.bind_sparse_batches;
         break;
      default:
         unreachable("unexpected batch type");
         break;
      }
   }

   uint32_t wait_sem_base = 0;
   uint32_t sync_base = 0;
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      if (submit->wait_wsi_count) {
         wait_sem_base += vn_queue_submission_filter_batch_wsi_semaphores(
            submit, i, wait_sem_base);
      }

      if (submit->signal_semaphore_count > submit->signal_device_only_count) {
         sync_base +=
            vn_queue_submission_setup_batch_syncs(submit, i, sync_base);
      } else if (submit->sync_count) {
         submit->temp.batch_sync_counts[i] = 0;
      }
   }

   sync_base += vn_queue_submission_setup_fence_sync(submit, sync_base);

   assert(sync_base == submit->sync_count);
}

static VkResult
vn_queue_submission_prepare_submit(struct vn_queue_submission *submit,
                                   VkQueue queue,
                                   uint32_t batch_count,
                                   const VkSubmitInfo *submit_batches,
                                   VkFence fence)
{
   submit->batch_type = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit->queue = queue;
   submit->batch_count = batch_count;
   submit->submit_batches = submit_batches;
   submit->fence = fence;

   vn_queue_submission_count_semaphores(submit);

   VkResult result = vn_queue_submission_alloc_storage(submit);
   if (result != VK_SUCCESS)
      return result;

   vn_queue_submission_setup_batches(submit);

   return VK_SUCCESS;
}

static VkResult
vn_queue_submission_prepare_bind_sparse(
   struct vn_queue_submission *submit,
   VkQueue queue,
   uint32_t batch_count,
   const VkBindSparseInfo *bind_sparse_batches,
   VkFence fence)
{
   submit->batch_type = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
   submit->queue = queue;
   submit->batch_count = batch_count;
   submit->bind_sparse_batches = bind_sparse_batches;
   submit->fence = fence;

   vn_queue_submission_count_semaphores(submit);

   VkResult result = vn_queue_submission_alloc_storage(submit);
   if (result != VK_SUCCESS)
      return result;

   vn_queue_submission_setup_batches(submit);

   return VK_SUCCESS;
}

static void
vn_queue_submission_cleanup(struct vn_queue_submission *submit)
{
   struct vn_queue *queue = vn_queue_from_handle(submit->queue);
   const VkAllocationCallbacks *alloc = &queue->device->base.base.alloc;

   vk_free(alloc, submit->temp.storage);
}

static void
vn_queue_submit_syncs(struct vn_queue *queue,
                      struct vn_renderer_sync *const *syncs,
                      const uint64_t *sync_values,
                      uint32_t sync_count)
{
   struct vn_instance *instance = queue->device->instance;
   const struct vn_renderer_submit_batch batch = {
      .sync_queue_index = queue->sync_queue_index,
      .vk_queue_id = queue->base.id,
      .syncs = syncs,
      .sync_values = sync_values,
      .sync_count = sync_count,
   };
   const struct vn_renderer_submit submit = {
      .batches = &batch,
      .batch_count = 1,
   };

   vn_renderer_submit(instance->renderer, &submit);
   vn_instance_roundtrip(instance);
}

VkResult
vn_QueueSubmit(VkQueue _queue,
               uint32_t submitCount,
               const VkSubmitInfo *pSubmits,
               VkFence fence)
{
   struct vn_queue *queue = vn_queue_from_handle(_queue);
   struct vn_device *dev = queue->device;

   struct vn_queue_submission submit;
   VkResult result = vn_queue_submission_prepare_submit(
      &submit, _queue, submitCount, pSubmits, fence);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* TODO this should be one trip to the renderer */
   if (submit.signal_timeline_count) {
      uint32_t sync_base = 0;
      for (uint32_t i = 0; i < submit.batch_count - 1; i++) {
         vn_async_vkQueueSubmit(dev->instance, submit.queue, 1,
                                &submit.submit_batches[i], VK_NULL_HANDLE);
         vn_instance_ring_wait(dev->instance);

         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.temp.batch_sync_counts[i]);
         sync_base += submit.temp.batch_sync_counts[i];
      }

      result = vn_call_vkQueueSubmit(
         dev->instance, submit.queue, 1,
         &submit.submit_batches[submit.batch_count - 1], submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (sync_base < submit.sync_count) {
         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.sync_count - sync_base);
      }
   } else {
      result = vn_call_vkQueueSubmit(dev->instance, submit.queue,
                                     submit.batch_count,
                                     submit.submit_batches, submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (submit.sync_count) {
         vn_queue_submit_syncs(queue, submit.temp.syncs,
                               submit.temp.sync_values, submit.sync_count);
      }
   }

   vn_queue_submission_cleanup(&submit);

   return VK_SUCCESS;
}

VkResult
vn_QueueBindSparse(VkQueue _queue,
                   uint32_t bindInfoCount,
                   const VkBindSparseInfo *pBindInfo,
                   VkFence fence)
{
   struct vn_queue *queue = vn_queue_from_handle(_queue);
   struct vn_device *dev = queue->device;

   struct vn_queue_submission submit;
   VkResult result = vn_queue_submission_prepare_bind_sparse(
      &submit, _queue, bindInfoCount, pBindInfo, fence);
   if (result != VK_SUCCESS)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* TODO this should be one trip to the renderer */
   if (submit.signal_timeline_count) {
      uint32_t sync_base = 0;
      for (uint32_t i = 0; i < submit.batch_count - 1; i++) {
         vn_async_vkQueueBindSparse(dev->instance, submit.queue, 1,
                                    &submit.bind_sparse_batches[i],
                                    VK_NULL_HANDLE);
         vn_instance_ring_wait(dev->instance);

         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.temp.batch_sync_counts[i]);
         sync_base += submit.temp.batch_sync_counts[i];
      }

      result = vn_call_vkQueueBindSparse(
         dev->instance, submit.queue, 1,
         &submit.bind_sparse_batches[submit.batch_count - 1], submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (sync_base < submit.sync_count) {
         vn_queue_submit_syncs(queue, &submit.temp.syncs[sync_base],
                               &submit.temp.sync_values[sync_base],
                               submit.sync_count - sync_base);
      }
   } else {
      result = vn_call_vkQueueBindSparse(
         dev->instance, submit.queue, submit.batch_count,
         submit.bind_sparse_batches, submit.fence);
      if (result != VK_SUCCESS) {
         vn_queue_submission_cleanup(&submit);
         return vn_error(dev->instance, result);
      }

      if (submit.sync_count) {
         vn_queue_submit_syncs(queue, submit.temp.syncs,
                               submit.temp.sync_values, submit.sync_count);
      }
   }

   vn_queue_submission_cleanup(&submit);

   return VK_SUCCESS;
}

VkResult
vn_QueueWaitIdle(VkQueue _queue)
{
   struct vn_queue *queue = vn_queue_from_handle(_queue);
   struct vn_device *dev = queue->device;
   struct vn_renderer *renderer = dev->instance->renderer;

   vn_instance_ring_wait(dev->instance);

   const uint64_t val = ++queue->idle_sync_value;
   const struct vn_renderer_submit submit = {
      .batches =
         &(const struct vn_renderer_submit_batch){
            .sync_queue_index = queue->sync_queue_index,
            .vk_queue_id = queue->base.id,
            .syncs = &queue->idle_sync,
            .sync_values = &val,
            .sync_count = 1,
         },
      .batch_count = 1,
   };
   vn_renderer_submit(renderer, &submit);

   const struct vn_renderer_wait wait = {
      .timeout = UINT64_MAX,
      .syncs = &queue->idle_sync,
      .sync_values = &val,
      .sync_count = 1,
   };
   VkResult result = vn_renderer_wait(renderer, &wait);

   return vn_result(dev->instance, result);
}

/* fence commands */

static void
vn_sync_payload_release(struct vn_device *dev,
                        struct vn_sync_payload *payload)
{
   if (payload->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_release(payload->sync);

   payload->type = VN_SYNC_TYPE_INVALID;
}

static VkResult
vn_fence_init_payloads(struct vn_device *dev,
                       struct vn_fence *fence,
                       bool signaled,
                       const VkAllocationCallbacks *alloc)
{
   struct vn_renderer_sync *perm_sync;
   VkResult result = vn_renderer_sync_create_fence(dev->instance->renderer,
                                                   signaled, 0, &perm_sync);
   if (result != VK_SUCCESS)
      return result;

   struct vn_renderer_sync *temp_sync;
   result =
      vn_renderer_sync_create_empty(dev->instance->renderer, &temp_sync);
   if (result != VK_SUCCESS) {
      vn_renderer_sync_destroy(perm_sync);
      return result;
   }

   fence->permanent.type = VN_SYNC_TYPE_SYNC;
   fence->permanent.sync = perm_sync;

   /* temp_sync is uninitialized */
   fence->temporary.type = VN_SYNC_TYPE_INVALID;
   fence->temporary.sync = temp_sync;

   fence->payload = &fence->permanent;

   return VK_SUCCESS;
}

void
vn_fence_signal_wsi(struct vn_device *dev, struct vn_fence *fence)
{
   struct vn_sync_payload *temp = &fence->temporary;

   vn_sync_payload_release(dev, temp);
   temp->type = VN_SYNC_TYPE_WSI_SIGNALED;
   fence->payload = temp;
}

VkResult
vn_CreateFence(VkDevice device,
               const VkFenceCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkFence *pFence)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   VkFenceCreateInfo local_create_info;
   if (vk_find_struct_const(pCreateInfo->pNext, EXPORT_FENCE_CREATE_INFO)) {
      local_create_info = *pCreateInfo;
      local_create_info.pNext = NULL;
      pCreateInfo = &local_create_info;
   }

   struct vn_fence *fence = vk_zalloc(alloc, sizeof(*fence), VN_DEFAULT_ALIGN,
                                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!fence)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&fence->base, VK_OBJECT_TYPE_FENCE, &dev->base);

   VkResult result = vn_fence_init_payloads(
      dev, fence, pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, fence);
      return vn_error(dev->instance, result);
   }

   VkFence fence_handle = vn_fence_to_handle(fence);
   vn_async_vkCreateFence(dev->instance, device, pCreateInfo, NULL,
                          &fence_handle);

   *pFence = fence_handle;

   return VK_SUCCESS;
}

void
vn_DestroyFence(VkDevice device,
                VkFence _fence,
                const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(_fence);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!fence)
      return;

   vn_async_vkDestroyFence(dev->instance, device, _fence, NULL);

   vn_sync_payload_release(dev, &fence->permanent);
   vn_sync_payload_release(dev, &fence->temporary);
   vn_renderer_sync_destroy(fence->permanent.sync);
   vn_renderer_sync_destroy(fence->temporary.sync);

   vn_object_base_fini(&fence->base);
   vk_free(alloc, fence);
}

VkResult
vn_ResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences)
{
   struct vn_device *dev = vn_device_from_handle(device);

   /* TODO if the fence is shared-by-ref, this needs to be synchronous */
   if (false)
      vn_call_vkResetFences(dev->instance, device, fenceCount, pFences);
   else
      vn_async_vkResetFences(dev->instance, device, fenceCount, pFences);

   for (uint32_t i = 0; i < fenceCount; i++) {
      struct vn_fence *fence = vn_fence_from_handle(pFences[i]);
      struct vn_sync_payload *perm = &fence->permanent;

      vn_sync_payload_release(dev, &fence->temporary);

      assert(perm->type == VN_SYNC_TYPE_SYNC);
      vn_renderer_sync_reset(perm->sync, 0);
      fence->payload = perm;
   }

   return VK_SUCCESS;
}

VkResult
vn_GetFenceStatus(VkDevice device, VkFence _fence)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(_fence);
   struct vn_sync_payload *payload = fence->payload;

   VkResult result;
   uint64_t val;
   switch (payload->type) {
   case VN_SYNC_TYPE_SYNC:
      result = vn_renderer_sync_read(payload->sync, &val);
      if (result == VK_SUCCESS && !val)
         result = VK_NOT_READY;
      break;
   case VN_SYNC_TYPE_WSI_SIGNALED:
      result = VK_SUCCESS;
      break;
   default:
      unreachable("unexpected fence payload type");
      break;
   }

   return vn_result(dev->instance, result);
}

VkResult
vn_WaitForFences(VkDevice device,
                 uint32_t fenceCount,
                 const VkFence *pFences,
                 VkBool32 waitAll,
                 uint64_t timeout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   struct vn_renderer_sync *local_syncs[8];
   uint64_t local_sync_vals[8];
   struct vn_renderer_sync **syncs = local_syncs;
   uint64_t *sync_vals = local_sync_vals;
   if (fenceCount > ARRAY_SIZE(local_syncs)) {
      syncs = vk_alloc(alloc, sizeof(*syncs) * fenceCount, VN_DEFAULT_ALIGN,
                       VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      sync_vals =
         vk_alloc(alloc, sizeof(*sync_vals) * fenceCount, VN_DEFAULT_ALIGN,
                  VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!syncs || !sync_vals) {
         vk_free(alloc, syncs);
         vk_free(alloc, sync_vals);
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   uint32_t wait_count = 0;
   uint32_t signaled_count = 0;
   for (uint32_t i = 0; i < fenceCount; i++) {
      struct vn_fence *fence = vn_fence_from_handle(pFences[i]);
      const struct vn_sync_payload *payload = fence->payload;

      switch (payload->type) {
      case VN_SYNC_TYPE_SYNC:
         syncs[wait_count] = payload->sync;
         sync_vals[wait_count] = 1;
         wait_count++;
         break;
      case VN_SYNC_TYPE_WSI_SIGNALED:
         signaled_count++;
         break;
      default:
         unreachable("unexpected fence payload type");
         break;
      }
   }

   VkResult result = VK_SUCCESS;
   if (wait_count && (waitAll || !signaled_count)) {
      const struct vn_renderer_wait wait = {
         .wait_any = !waitAll,
         .timeout = timeout,
         .syncs = syncs,
         .sync_values = sync_vals,
         .sync_count = wait_count,
      };
      result = vn_renderer_wait(dev->instance->renderer, &wait);
   }

   if (syncs != local_syncs) {
      vk_free(alloc, syncs);
      vk_free(alloc, sync_vals);
   }

   return vn_result(dev->instance, result);
}

VkResult
vn_ImportFenceFdKHR(VkDevice device,
                    const VkImportFenceFdInfoKHR *pImportFenceFdInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(pImportFenceFdInfo->fence);
   const bool sync_file = pImportFenceFdInfo->handleType ==
                          VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
   const int fd = pImportFenceFdInfo->fd;
   struct vn_sync_payload *payload =
      pImportFenceFdInfo->flags & VK_FENCE_IMPORT_TEMPORARY_BIT
         ? &fence->temporary
         : &fence->permanent;

   if (payload->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_release(payload->sync);

   VkResult result;
   if (sync_file && fd < 0)
      result = vn_renderer_sync_init_signaled(payload->sync);
   else
      result = vn_renderer_sync_init_syncobj(payload->sync, fd, sync_file);

   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   payload->type = VN_SYNC_TYPE_SYNC;
   fence->payload = payload;

   if (fd >= 0)
      close(fd);

   return VK_SUCCESS;
}

VkResult
vn_GetFenceFdKHR(VkDevice device,
                 const VkFenceGetFdInfoKHR *pGetFdInfo,
                 int *pFd)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_fence *fence = vn_fence_from_handle(pGetFdInfo->fence);
   const bool sync_file =
      pGetFdInfo->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
   struct vn_sync_payload *payload = fence->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   int fd = vn_renderer_sync_export_syncobj(payload->sync, sync_file);
   if (fd < 0)
      return vn_error(dev->instance, VK_ERROR_TOO_MANY_OBJECTS);

   if (sync_file)
      vn_ResetFences(device, 1, &pGetFdInfo->fence);

   *pFd = fd;
   return VK_SUCCESS;
}

/* semaphore commands */

static VkResult
vn_semaphore_init_payloads(struct vn_device *dev,
                           struct vn_semaphore *sem,
                           uint64_t initial_val,
                           const VkAllocationCallbacks *alloc)
{
   struct vn_renderer_sync *perm_sync;
   VkResult result;
   if (sem->type == VK_SEMAPHORE_TYPE_TIMELINE) {
      result = vn_renderer_sync_create_semaphore(dev->instance->renderer,
                                                 VK_SEMAPHORE_TYPE_TIMELINE,
                                                 initial_val, 0, &perm_sync);
   } else {
      result =
         vn_renderer_sync_create_empty(dev->instance->renderer, &perm_sync);
   }
   if (result != VK_SUCCESS)
      return result;

   struct vn_renderer_sync *temp_sync;
   result =
      vn_renderer_sync_create_empty(dev->instance->renderer, &temp_sync);
   if (result != VK_SUCCESS) {
      vn_renderer_sync_destroy(perm_sync);
      return result;
   }

   sem->permanent.type = sem->type == VK_SEMAPHORE_TYPE_TIMELINE
                            ? VN_SYNC_TYPE_SYNC
                            : VN_SYNC_TYPE_DEVICE_ONLY;
   sem->permanent.sync = perm_sync;

   /* temp_sync is uninitialized */
   sem->temporary.type = VN_SYNC_TYPE_INVALID;
   sem->temporary.sync = temp_sync;

   sem->payload = &sem->permanent;

   return VK_SUCCESS;
}

static void
vn_semaphore_reset_wsi(struct vn_device *dev, struct vn_semaphore *sem)
{
   struct vn_sync_payload *perm = &sem->permanent;

   vn_sync_payload_release(dev, &sem->temporary);

   if (perm->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_reset(perm->sync, 0);
   sem->payload = perm;
}

void
vn_semaphore_signal_wsi(struct vn_device *dev, struct vn_semaphore *sem)
{
   struct vn_sync_payload *temp = &sem->temporary;

   vn_sync_payload_release(dev, temp);
   temp->type = VN_SYNC_TYPE_WSI_SIGNALED;
   sem->payload = temp;
}

VkResult
vn_CreateSemaphore(VkDevice device,
                   const VkSemaphoreCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkSemaphore *pSemaphore)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   struct vn_semaphore *sem = vk_zalloc(alloc, sizeof(*sem), VN_DEFAULT_ALIGN,
                                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!sem)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vn_object_base_init(&sem->base, VK_OBJECT_TYPE_SEMAPHORE, &dev->base);

   const VkSemaphoreTypeCreateInfo *type_info =
      vk_find_struct_const(pCreateInfo->pNext, SEMAPHORE_TYPE_CREATE_INFO);
   uint64_t initial_val = 0;
   if (type_info && type_info->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
      sem->type = VK_SEMAPHORE_TYPE_TIMELINE;
      initial_val = type_info->initialValue;
   } else {
      sem->type = VK_SEMAPHORE_TYPE_BINARY;
   }

   VkResult result = vn_semaphore_init_payloads(dev, sem, initial_val, alloc);
   if (result != VK_SUCCESS) {
      vk_free(alloc, sem);
      return vn_error(dev->instance, result);
   }

   VkSemaphore sem_handle = vn_semaphore_to_handle(sem);
   vn_async_vkCreateSemaphore(dev->instance, device, pCreateInfo, NULL,
                              &sem_handle);

   *pSemaphore = sem_handle;

   return VK_SUCCESS;
}

void
vn_DestroySemaphore(VkDevice device,
                    VkSemaphore semaphore,
                    const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem = vn_semaphore_from_handle(semaphore);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.base.alloc;

   if (!sem)
      return;

   vn_async_vkDestroySemaphore(dev->instance, device, semaphore, NULL);

   vn_sync_payload_release(dev, &sem->permanent);
   vn_sync_payload_release(dev, &sem->temporary);
   vn_renderer_sync_destroy(sem->permanent.sync);
   vn_renderer_sync_destroy(sem->temporary.sync);

   vn_object_base_fini(&sem->base);
   vk_free(alloc, sem);
}

VkResult
vn_GetSemaphoreCounterValue(VkDevice device,
                            VkSemaphore semaphore,
                            uint64_t *pValue)
{
   struct vn_semaphore *sem = vn_semaphore_from_handle(semaphore);
   struct vn_sync_payload *payload = sem->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   return vn_renderer_sync_read(payload->sync, pValue);
}

VkResult
vn_SignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem =
      vn_semaphore_from_handle(pSignalInfo->semaphore);
   struct vn_sync_payload *payload = sem->payload;

   /* TODO if the semaphore is shared-by-ref, this needs to be synchronous */
   if (false)
      vn_call_vkSignalSemaphore(dev->instance, device, pSignalInfo);
   else
      vn_async_vkSignalSemaphore(dev->instance, device, pSignalInfo);

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   vn_renderer_sync_write(payload->sync, pSignalInfo->value);

   return VK_SUCCESS;
}

VkResult
vn_WaitSemaphores(VkDevice device,
                  const VkSemaphoreWaitInfo *pWaitInfo,
                  uint64_t timeout)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc = &dev->base.base.alloc;

   struct vn_renderer_sync *local_syncs[8];
   struct vn_renderer_sync **syncs = local_syncs;
   if (pWaitInfo->semaphoreCount > ARRAY_SIZE(local_syncs)) {
      syncs = vk_alloc(alloc, sizeof(*syncs) * pWaitInfo->semaphoreCount,
                       VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!syncs)
         return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
      struct vn_semaphore *sem =
         vn_semaphore_from_handle(pWaitInfo->pSemaphores[i]);
      const struct vn_sync_payload *payload = sem->payload;

      assert(payload->type == VN_SYNC_TYPE_SYNC);
      syncs[i] = payload->sync;
   }

   const struct vn_renderer_wait wait = {
      .wait_any = pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT,
      .timeout = timeout,
      .syncs = syncs,
      .sync_values = pWaitInfo->pValues,
      .sync_count = pWaitInfo->semaphoreCount,
   };
   VkResult result = vn_renderer_wait(dev->instance->renderer, &wait);

   if (syncs != local_syncs)
      vk_free(alloc, syncs);

   return vn_result(dev->instance, result);
}

VkResult
vn_ImportSemaphoreFdKHR(
   VkDevice device, const VkImportSemaphoreFdInfoKHR *pImportSemaphoreFdInfo)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem =
      vn_semaphore_from_handle(pImportSemaphoreFdInfo->semaphore);
   const bool sync_file = pImportSemaphoreFdInfo->handleType ==
                          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
   const int fd = pImportSemaphoreFdInfo->fd;
   struct vn_sync_payload *payload =
      pImportSemaphoreFdInfo->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT
         ? &sem->temporary
         : &sem->permanent;

   if (payload->type == VN_SYNC_TYPE_SYNC)
      vn_renderer_sync_release(payload->sync);

   VkResult result;
   if (sync_file && fd < 0)
      result = vn_renderer_sync_init_signaled(payload->sync);
   else
      result = vn_renderer_sync_init_syncobj(payload->sync, fd, sync_file);

   if (result != VK_SUCCESS)
      return vn_error(dev->instance, result);

   /* TODO import into the host-side semaphore */

   payload->type = VN_SYNC_TYPE_SYNC;
   sem->payload = payload;

   if (fd >= 0)
      close(fd);

   return VK_SUCCESS;
}

VkResult
vn_GetSemaphoreFdKHR(VkDevice device,
                     const VkSemaphoreGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_semaphore *sem = vn_semaphore_from_handle(pGetFdInfo->semaphore);
   const bool sync_file =
      pGetFdInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
   struct vn_sync_payload *payload = sem->payload;

   assert(payload->type == VN_SYNC_TYPE_SYNC);
   int fd = vn_renderer_sync_export_syncobj(payload->sync, sync_file);
   if (fd < 0)
      return vn_error(dev->instance, VK_ERROR_TOO_MANY_OBJECTS);

   if (sync_file) {
      vn_sync_payload_release(dev, &sem->temporary);
      vn_renderer_sync_reset(sem->permanent.sync, 0);
      sem->payload = &sem->permanent;
      /* TODO reset the host-side semaphore */
   }

   *pFd = fd;
   return VK_SUCCESS;
}