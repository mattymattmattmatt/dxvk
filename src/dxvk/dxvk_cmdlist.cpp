#include <atomic>
#include <chrono>
#include <thread>

#include "dxvk_cmdlist.h"
#include "dxvk_device.h"

// GESVR: d3d9_vr.cpp, set through config FakeSubmitOOM (L4D2VR/vr.h).
extern std::atomic<int> g_GESVR_FakeSubmitOOM;

namespace dxvk {
    
  DxvkCommandList::DxvkCommandList(DxvkDevice* device)
  : m_device        (device),
    m_vkd           (device->vkd()),
    m_vki           (device->instance()->vki()),
    m_cmdBuffersUsed(0),
    m_descriptorPoolTracker(device) {
    const auto& graphicsQueue = m_device->queues().graphics;
    const auto& transferQueue = m_device->queues().transfer;

    VkFenceCreateInfo fenceInfo;
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = 0;
    
    if (m_vkd->vkCreateFence(m_vkd->device(), &fenceInfo, nullptr, &m_fence) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to create fence");
    
    VkCommandPoolCreateInfo poolInfo;
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.pNext            = nullptr;
    poolInfo.flags            = 0;
    poolInfo.queueFamilyIndex = graphicsQueue.queueFamily;
    
    if (m_vkd->vkCreateCommandPool(m_vkd->device(), &poolInfo, nullptr, &m_graphicsPool) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to create graphics command pool");
    
    if (m_device->hasDedicatedTransferQueue()) {
      poolInfo.queueFamilyIndex = transferQueue.queueFamily;

      if (m_vkd->vkCreateCommandPool(m_vkd->device(), &poolInfo, nullptr, &m_transferPool) != VK_SUCCESS)
        throw DxvkError("DxvkCommandList: Failed to create transfer command pool");
    }
    
    VkCommandBufferAllocateInfo cmdInfoGfx;
    cmdInfoGfx.sType             = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfoGfx.pNext             = nullptr;
    cmdInfoGfx.commandPool       = m_graphicsPool;
    cmdInfoGfx.level             = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfoGfx.commandBufferCount = 1;
    
    VkCommandBufferAllocateInfo cmdInfoDma;
    cmdInfoDma.sType             = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfoDma.pNext             = nullptr;
    cmdInfoDma.commandPool       = m_transferPool ? m_transferPool : m_graphicsPool;
    cmdInfoDma.level             = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfoDma.commandBufferCount = 1;
    
    if (m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoGfx, &m_execBuffer) != VK_SUCCESS
     || m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoGfx, &m_initBuffer) != VK_SUCCESS
     || m_vkd->vkAllocateCommandBuffers(m_vkd->device(), &cmdInfoDma, &m_sdmaBuffer) != VK_SUCCESS)
      throw DxvkError("DxvkCommandList: Failed to allocate command buffer");
    
    if (m_device->hasDedicatedTransferQueue()) {
      VkSemaphoreCreateInfo semInfo;
      semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semInfo.pNext = nullptr;
      semInfo.flags = 0;

      if (m_vkd->vkCreateSemaphore(m_vkd->device(), &semInfo, nullptr, &m_sdmaSemaphore) != VK_SUCCESS)
        throw DxvkError("DxvkCommandList: Failed to create semaphore");
    }
  }
  
  
  DxvkCommandList::~DxvkCommandList() {
    this->reset();

    m_vkd->vkDestroySemaphore(m_vkd->device(), m_sdmaSemaphore, nullptr);
    
    m_vkd->vkDestroyCommandPool(m_vkd->device(), m_graphicsPool, nullptr);
    m_vkd->vkDestroyCommandPool(m_vkd->device(), m_transferPool, nullptr);
    
    m_vkd->vkDestroyFence(m_vkd->device(), m_fence, nullptr);
  }
  
  
  VkResult DxvkCommandList::submit(
          VkSemaphore     waitSemaphore,
          VkSemaphore     wakeSemaphore) {
    const auto& graphics = m_device->queues().graphics;
    const auto& transfer = m_device->queues().transfer;

    DxvkQueueSubmission info = DxvkQueueSubmission();

    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::SdmaBuffer)) {
      info.cmdBuffers[info.cmdBufferCount++] = m_sdmaBuffer;

      if (m_device->hasDedicatedTransferQueue()) {
        info.wakeSync[info.wakeCount++] = m_sdmaSemaphore;
        VkResult status = submitToQueue(transfer.queueHandle, VK_NULL_HANDLE, info);

        if (status != VK_SUCCESS)
          return status;

        info = DxvkQueueSubmission();
        info.waitSync[info.waitCount] = m_sdmaSemaphore;
        info.waitMask[info.waitCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        info.waitCount += 1;
      }
    }

    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::InitBuffer))
      info.cmdBuffers[info.cmdBufferCount++] = m_initBuffer;
    if (m_cmdBuffersUsed.test(DxvkCmdBuffer::ExecBuffer))
      info.cmdBuffers[info.cmdBufferCount++] = m_execBuffer;
    
    if (waitSemaphore) {
      info.waitSync[info.waitCount] = waitSemaphore;
      info.waitMask[info.waitCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      info.waitCount += 1;
    }

    if (wakeSemaphore)
      info.wakeSync[info.wakeCount++] = wakeSemaphore;
    
    return submitToQueue(graphics.queueHandle, m_fence, info);
  }
  
  
  VkResult DxvkCommandList::synchronize() {
    VkResult status = VK_TIMEOUT;
    
    while (status == VK_TIMEOUT) {
      status = m_vkd->vkWaitForFences(
        m_vkd->device(), 1, &m_fence, VK_FALSE,
        1'000'000'000ull);
    }
    
    return status;
  }
  
  
  void DxvkCommandList::beginRecording() {
    VkCommandBufferBeginInfo info;
    info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.pNext            = nullptr;
    info.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    info.pInheritanceInfo = nullptr;
    
    if ((m_graphicsPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_graphicsPool, 0) != VK_SUCCESS)
     || (m_transferPool && m_vkd->vkResetCommandPool(m_vkd->device(), m_transferPool, 0) != VK_SUCCESS))
      Logger::err("DxvkCommandList: Failed to reset command buffer");
    
    if (m_vkd->vkBeginCommandBuffer(m_execBuffer, &info) != VK_SUCCESS
     || m_vkd->vkBeginCommandBuffer(m_initBuffer, &info) != VK_SUCCESS
     || m_vkd->vkBeginCommandBuffer(m_sdmaBuffer, &info) != VK_SUCCESS)
      Logger::err("DxvkCommandList: Failed to begin command buffer");
    
    if (m_vkd->vkResetFences(m_vkd->device(), 1, &m_fence) != VK_SUCCESS)
      Logger::err("DxvkCommandList: Failed to reset fence");
    
    // Unconditionally mark the exec buffer as used. There
    // is virtually no use case where this isn't correct.
    m_cmdBuffersUsed = DxvkCmdBuffer::ExecBuffer;
  }
  
  
  void DxvkCommandList::endRecording() {
    if (m_vkd->vkEndCommandBuffer(m_execBuffer) != VK_SUCCESS
     || m_vkd->vkEndCommandBuffer(m_initBuffer) != VK_SUCCESS
     || m_vkd->vkEndCommandBuffer(m_sdmaBuffer) != VK_SUCCESS)
      Logger::err("DxvkCommandList::endRecording: Failed to record command buffer");
  }
  
  
  void DxvkCommandList::reset() {
    // Free resources and other objects
    // that are no longer in use
    m_resources.reset();

    // Recycle heavy Vulkan objects
    m_descriptorPoolTracker.reset();

    // Return buffer memory slices
    m_bufferTracker.reset();

    // Return query and event handles
    m_gpuQueryTracker.reset();
    m_gpuEventTracker.reset();

    // Less important stuff
    m_signalTracker.reset();
    m_statCounters.reset();
  }


  VkResult DxvkCommandList::submitToQueue(
          VkQueue               queue,
          VkFence               fence,
    const DxvkQueueSubmission&  info) {
    VkSubmitInfo submitInfo;
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = nullptr;
    submitInfo.waitSemaphoreCount   = info.waitCount;
    submitInfo.pWaitSemaphores      = info.waitSync;
    submitInfo.pWaitDstStageMask    = info.waitMask;
    submitInfo.commandBufferCount   = info.cmdBufferCount;
    submitInfo.pCommandBuffers      = info.cmdBuffers;
    submitInfo.signalSemaphoreCount = info.wakeCount;
    submitInfo.pSignalSemaphores    = info.wakeSync;

    auto queueSubmit = [&] () -> VkResult {
      // GESVR_FakeSubmitOOM test hook: fail without reaching the driver.
      int fake = g_GESVR_FakeSubmitOOM.load();
      while (fake > 0) {
        if (g_GESVR_FakeSubmitOOM.compare_exchange_weak(fake, fake - 1))
          return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      return m_vkd->vkQueueSubmit(queue, 1, &submitInfo, fence);
    };

    VkResult status = queueSubmit();

    // GESVR: hl2.exe is a 32-bit process with 2 GB of address space, and a map
    // load takes it to within ~100 MB of the ceiling while the pipeline
    // compilers hold large temporary allocations. A submit that fails for
    // memory then is likely to succeed a moment later, and the spec makes the
    // retry safe: on OUT_OF_*_MEMORY vkQueueSubmit must leave every resource,
    // fence and semaphore it references untouched (anything it cannot undo is
    // reported as DEVICE_LOST instead). Retrying HERE, per vkQueueSubmit, keeps
    // a two-queue submission from re-sending the half that already went.
    // Up to ~2 s; the queue lock is held throughout, which stalls the OpenVR
    // submit on the render thread for that long and nothing worse.
    for (uint32_t attempt = 1; attempt <= 40
        && (status == VK_ERROR_OUT_OF_HOST_MEMORY || status == VK_ERROR_OUT_OF_DEVICE_MEMORY); attempt++) {
      if (attempt == 1)
        Logger::warn(str::format("DxvkCommandList: vkQueueSubmit out of memory (", status, "), retrying"));
      std::this_thread::sleep_for(std::chrono::milliseconds(attempt < 10 ? 10 : 50));
      status = queueSubmit();
      if (status == VK_SUCCESS)
        Logger::warn(str::format("DxvkCommandList: vkQueueSubmit succeeded on retry ", attempt));
    }

    return status;
  }
  
  void DxvkCommandList::cmdBeginDebugUtilsLabel(VkDebugUtilsLabelEXT *pLabelInfo) {
    m_vki->vkCmdBeginDebugUtilsLabelEXT(m_execBuffer, pLabelInfo);
  }

  void DxvkCommandList::cmdEndDebugUtilsLabel() {
    m_vki->vkCmdEndDebugUtilsLabelEXT(m_execBuffer);
  }

  void DxvkCommandList::cmdInsertDebugUtilsLabel(VkDebugUtilsLabelEXT *pLabelInfo) {
    m_vki->vkCmdInsertDebugUtilsLabelEXT(m_execBuffer, pLabelInfo);
  }
}
