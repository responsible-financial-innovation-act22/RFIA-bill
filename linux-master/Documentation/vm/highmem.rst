.. _highmem:

====================
High Memory Handling
====================

By: Peter Zijlstra <a.p.zijlstra@chello.nl>

.. contents:: :local:

What Is High Memory?
====================

High memory (highmem) is used when the size of physical memory approaches or
exceeds the maximum size of virtual memory.  At that point it becomes
impossible for the kernel to keep all of the available physical memory mapped
at all times.  This means the kernel needs to start using temporary mappings of
the pieces of physical memory that it wants to access.

The part of (physical) memory not covered by a permanent mapping is what we
refer to as 'highmem'.  There are various architecture dependent constraints on
where exactly that border lies.

In the i386 arch, for example, we choose to map the kernel into every process's
VM space so that we don't have to pay the full TLB invalidation costs for
kernel entry/exit.  This means the available virtual memory space (4GiB on
i386) has to be divided between user and kernel space.

The traditional split for architectures using this approach is 3:1, 3GiB for
userspace and the top 1GiB for kernel space::

		+--------+ 0xffffffff
		| Kernel |
		+--------+ 0xc0000000
		|        |
		| User   |
		|        |
		+--------+ 0x00000000

This means that the kernel can at most map 1GiB of physical memory at any one
time, but because we need virtual address space for other things - including
temporary maps to access the rest of the physical memory - the actual direct
map will typically be less (usually around ~896MiB).

Other architectures that have mm context tagged TLBs can have separate kernel
and user maps.  Some hardware (like some ARMs), however, have limited virtual
space when they use mm context tags.


Temporary Virtual Mappings
==========================

The kernel contains several ways of creating temporary mappings. The following
list shows them in order of preference of use.

* kmap_local_page().  This function is used to require short term mappings.
  It can be invoked from any context (including interrupts) but the mappings
  can only be used in the context which acquired them.

  This function should be preferred, where feasible, over all the others.

  These mappings are thread-local and CPU-local, meaning that the mapping
  can only be accessed from within this thread and the thread is bound the
  CPU while the mapping is active. Even if the thread is preempted (since
  preemption is never disabled by the function) the CPU can not be
  unplugged from the system via CPU-hotplug until the mapping is disposed.

  It's valid to take pagefaults in a local kmap region, unless the context
  in which the local mapping is acquired does not allow it for other reasons.

  kmap_local_page() always returns a valid virtual address and it is assumed
  that kunmap_local() will never fail.

  Nesting kmap_local_page() and kmap_atomic() mappings is allowed to a certain
  extent (up to KMAP_TYPE_NR) but their invocations have to be strictly ordered
  because the map implementation is stack based. See kmap_local_page() kdocs
  (included in the "Functions" section) for details on how to manage nested
  mappings.

* kmap_atomic().  This permits a very short duration mapping of a single
  page.  Since the mapping is restricted to the CPU that issued it, it
  performs well, but the issuing task is therefore required to stay on that
  CPU until it has finished, lest some other task displace its mappings.

  kmap_atomic() may also be used by interrupt contexts, since it does not
  sleep and the callers too may not sleep until after kunmap_atomic() is
  called.

  Each call of kmap_atomic() in the kernel creates a non-preemptible section
  and disable pagefaults. This could be a source of unwanted latency. Therefore
  users should prefer kmap_local_page() instead of kmap_atomic().

  It is assumed that k[un]map_atomic() won't fail.

* kmap().  This should be used to make short duration mapping of a single
  page with no restrictions on preemption or migration. It comes with an
  overhead as mapping space is restricted and protected by a global lock
  for synchronization. When mapping is no longer needed, the address that
  the page was mapped to must be released with kunmap().

  Mapping changes must be propagated across all the CPUs. kmap() also
  requires global TLB invalidation when the kmap's pool wraps and it might
  block when the mapping space is fully utilized until a slot becomes
  available. Therefore, kmap() is only callable from preemptible context.

  All the above work is necessary if a mapping must last for a relatively
  long time but the bulk of high-memory mappings in the kernel are
  short-lived and only used in one place. This means that the cost of
  kmap() is mostly wasted in such cases. kmap() was not intended for long
  term mappings but it has morphed in that direction and its use is
  strongly discouraged in newer code and the set of the preceding functions
  should be preferred.

  On 64-bit systems, calls to kmap_local_page(), kmap_atomic() and kmap() have
  no real work to do because a 64-bit address space is more than sufficient to
  address all the physical memory whose pages are permanently mapped.

* vmap().  This can be used to make a long duration mapping of multiple
  physical pages into a contiguous virtual space.  It needs global
  synchronization to unmap.


Cost of Temporary Mappings
==========================

The cost of creating temporary mappings can be quite high.  The arch has to
manipulate the kernel's page tables, the data TLB and/or the MMU's registers.

If CONFIG_HIGHMEM is not set, then the kernel will try and create a mapping
simply with a bit of arithmetic that will convert the page struct address into
a pointer to the page contents rather than juggling mappings about.  In such a
case, the unmap operation may be a null operation.

If CONFIG_MMU is not set, then there can be no temporary mappings and no
highmem.  In such a case, the arithmetic approach will also be used.


i386 PAE
========

The i386 arch, under some circumstances, will permit you to stick up to 64GiB
of RAM into your 32-bit machine.  This has a number of consequences:

* Linux needs a page-frame structure for each page in the system and the
  pageframes need to live in the permanent mapping, which means:

* you can have 896M/sizeof(struct page) page-frames at most; with struct
  page being 32-bytes that would end up being something in the order of 112G
  worth of pages; the kernel, however, needs to store more than just
  page-frames in that memory...

* PAE makes your page tables larger - which slows the system down as more
  data has to be accessed to traverse in TLB fills and the like.  One
  advantage is that PAE has more PTE bits and can provide advanced features
  like NX and PAT.

The general recommendation is that you don't use more than 8GiB on a 32-bit
machine - although more might work for you and your workload, you're pretty
much on your own - don't expect kernel developers to really care much if things
come apart.


Functions
=========

.. kernel-doc:: include/linux/highmem.h
.. kernel-doc:: include/linux/highmem-internal.h
