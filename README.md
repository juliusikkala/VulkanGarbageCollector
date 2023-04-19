VKGC - Vulkan Garbage Collector
===============================

A simple, **public domain** C++11 thread-safe garbage collector for Vulkan
resources. It has no dependencies other than Vulkan and the standard library.

One common pattern for deleting Vulkan resources is a deletion queue that
advances on each frame, removing resources that were last used in frames which
are no longer in flight. However, that approach is hard to apply to async
computations, where you do not know which frame the computation will finish on
ahead of time.

The garbage collection approach is inevitably heavier on the CPU, but does not
depend on frame-based operation and is good at ensuring the correct deletion
order for everything, as long as dependencies are marked correctly.

## Usage

First, make sure the GC is available everywhere in your Vulkan code, likely as a
member of some context or device class if you have one. Use `std::optional<>` to
get around RAII if you have issues with getting the logical device before
calling the constructor of `garbage_collector`.
```c++
vkgc::garbage_collector gc(my_logical_device);
```

Then, start marking resource dependencies and call `release()` once you are done
with the resources. In practice, you probably want to write wrappers for the
Vulkan resource types that call these releases automatically in the destructor.

```c++
VkSemaphore sem = create_timeline_semaphore();
VkPipeline pipeline = create_discombobulation_compute_pipeline();
VkCommandBuffer cmd = create_command_buffer();
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, my_pipeline);

// The command buffer now uses the pipeline, so we have to mark that to the GC.
gc.depend(pipeline, cmd);

// ...  more command here  ...

submit_command_buffer_and_signal(cmd, sem, 1337);

// After vkQueueSubmit(), you need to mark that the command buffer is being used
// until the given value is reached.
gc.depend(cmd, sem, 1337);

// Now that dependencies are marked, you can release all resources and forget
// about their existence.
gc.release(cmd, [=](){vkFreeCommandBuffers(my_logical_device, pool, 1, &cmd);});
gc.release(pipeline, [=](){vkDestroyPipeline(my_logical_device, pipeline, nullptr);});
gc.release(sem);
```

Additionally, `depend_many()` can be used to add a bunch of dependencies in one
go, which can be useful in certain cases, especially with descriptor sets
referencing an array of bindless textures. `add_trigger()` can be used to add
an arbitrary callback to when a tracked timeline semaphore reaches a given
value.

Then, in the main loop, or otherwise periodically, call `collect()` in order to
actually deallocate unused resources:

```c++
gc.collect();
```

At the very end of the program, you may want to call `gc.wait_collect()` to
ensure that everything in the GC gets removed.

## Thread-safety

`depend()`, `depend_many()`, `add_trigger()`, `collect()`, `wait_collect()` and
`release()` are safe to call from any thread. You will not gain a performance
benefit from doing so, though.

Note that Vulkan itself adds a thread-safety gotcha: `VkCommandPool` may not
be used from multiple threads simultaneously, so you likely can't just
call `vkFreeCommandBuffers()` in the cleanup callback of `release()` in a program
that uses Vulkan from multiple threads. I've usually solved this by creating
command pools per thread and collecting expired command buffers into an array
that gets freed whenever new command buffers are allocated from the related
pool.

## Modifying

This "library" is meant to be modified to fit your codebase. The first thing you
probably want to do, is swap the containers (`std::vector`,
`std::unordered_map`, and `std::priority_queue`) to whatever more efficient
ones your engine uses. Small vector optimization and better hash maps should
be a big performance gain.

## Integration

Copy `vkgc.cc` and `vkgc.hh` to your project and add them to your build system
of choice. Modify the `vkgc.hh` to include whichever Vulkan header you happen
to use (likely `vulkan/vulkan.h` or `volk.h`).

Unfortunately, gradually adding a garbage collector to an existing Vulkan
codebase is quite hard, but can be done. Assuming you have an existing scheme
for releasing resources, here's my recommended order of moving resources over
to the GC:

1.  `VkSemaphore`
2.  `VkCommandBuffer`
3.  `VkCommandPool`
4.  `VkQueryPool`
5.  `VkPipeline`
6.  `VkPipelineLayout`
7.  `VkPipelineCache`
8.  `VkDescriptorSet`
9.  `VkDescriptorUpdateTemplate`
10. `VkDescriptorPool`
11. `VkDescriptorSetLayout`
12. `VkAccelerationStructureKHR`
13. `VkRenderPass`
14. `VkShaderModule`
15. `VkFramebuffer`
16. `VkSampler`
17. `VkImageView`
18. `VkImage`
19. `VkBufferView`
20. `VkBuffer`

The idea is that the earlier resources in the list are users of the latter
resources, so it should safe to turn over their control to the GC in this order,
as long as you always run the GC just before running your previous scheme.
