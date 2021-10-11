/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;
bool coremap_created = 0;
int num_page;
static paddr_t low;
static paddr_t high;




void
vm_bootstrap(void)
{
	/* Do nothing. */
	#if OPT_A3
	ram_getsize(&low, &high);

	num_page = (high - low) / PAGE_SIZE;

	for(int i = 0; i < num_page; i++) {
		((int *)PADDR_TO_KVADDR(low))[i] = 0;
	}

	coremap_created = 1;
	#endif
}

paddr_t
coremap_stealmem(unsigned long npages)   // might have issue
{
	for(int i = 0; i < num_page; i++) {
		int p = 0;
		int start = i;
		int val = ((int*) PADDR_TO_KVADDR(low))[i];

		while(val == 0 && start + p < num_page) {
			p++;

			if(p == (int) npages) {
				start = i;
				for(int j = 1; j < (int) npages + 1; j++) {
					((int *) PADDR_TO_KVADDR(low))[start] = j;
					start++;
				}


				paddr_t addr = low + (i + 1) * PAGE_SIZE;
				// kprintf("addr is %d\n", addr);
				return addr;
			}

			
			val = ((int*) PADDR_TO_KVADDR(low))[start + p];
		}
	}

	return 0;
}



static
paddr_t
getppages(unsigned long npages)
{	

	paddr_t addr;
	#if OPT_A3
	if(coremap_created == 1) {
		

		spinlock_acquire(&coremap_lock);

		addr = coremap_stealmem(npages);
	
		spinlock_release(&coremap_lock);

		return addr;
	} else {
		

		spinlock_acquire(&stealmem_lock);

		addr = ram_stealmem(npages);
	
		spinlock_release(&stealmem_lock);

		return addr;

	}
	#else
	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);

		return addr;
	#endif
	
	
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr) // has nothing to do with the bug
{
	/* nothing - leak the memory. */

	#if OPT_A3
	spinlock_acquire(&coremap_lock);
	
	


	// KASSERT(physical_addr % PAGE_SIZE == 0);

	if(coremap_created == true) {
		
		int index = ((addr - MIPS_KSEG0 - low) / PAGE_SIZE) - 1;

		((int *)PADDR_TO_KVADDR(low))[index] = 0;
		
		int it = ((int *)PADDR_TO_KVADDR(low))[++index];

		while(it > 1) {
			((int *)PADDR_TO_KVADDR(low))[index] = 0;
			it = ((int *)PADDR_TO_KVADDR(low))[++index];
		}
	}

	spinlock_release(&coremap_lock);

	#else
	(void)addr;
	#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		#if OPT_A3
		return EFAULT;
		#else
		panic("dumbvm: got VM_FAULT_READONLY\n");
		#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */

	#if OPT_A3
	KASSERT(as->as_pbase1 != NULL);
	KASSERT(as->as_pbase2 != NULL);
	KASSERT(as->as_stackpbase != NULL);

	#else
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_stackpbase != 0);
	#endif

	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	
	#if OPT_A3
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase1[0] & PAGE_FRAME) == as->as_pbase1[0]);
	KASSERT((as->as_pbase2[0] & PAGE_FRAME) == as->as_pbase2[0]);
	KASSERT((as->as_stackpbase[0] & PAGE_FRAME) == as->as_stackpbase[0]);

	#else
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
	#endif


	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	bool read_only = false; 

	#if OPT_A3
	
	// if (faultaddress >= vbase1 && faultaddress < vtop1) {
	// 	int page_num = (faultaddress - vbase1) / PAGE_SIZE;
	// 	int remainder = (faultaddress - vbase1) % PAGE_SIZE;
	// 	paddr = as->as_pbase1[page_num] + remainder;
	// 	read_only = true; // it is text segment, which should be read-only
	// }
	// else if (faultaddress >= vbase2 && faultaddress < vtop2) {
	// 	int page_num = (faultaddress - vbase2) / PAGE_SIZE;
	// 	int remainder = (faultaddress - vbase2) % PAGE_SIZE;
	// 	paddr = as->as_pbase2[page_num] + remainder;
	// }
	// else if (faultaddress >= stackbase && faultaddress < stacktop) {
	// 	int page_num = (faultaddress - stackbase) / PAGE_SIZE;
	// 	int remainder = (faultaddress - stackbase) % PAGE_SIZE;
	// 	paddr = as->as_stackpbase[page_num] + remainder;
	// }
	// else {
	// 	return EFAULT;
	// }
	// kprintf("%d", paddr);

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		// kprintf("code segment:\n");
		// kprintf("faultaddress - vbase1 is %d\n", (faultaddress - vbase1));
		// kprintf("faultaddress - vbase1 & PAGE_FRAME is %d\n", (faultaddress - vbase1) & PAGE_FRAME);
		// kprintf("as_pbase1[0] is %d\n", as->as_pbase1[0]);
		paddr = (faultaddress - vbase1) + as->as_pbase1[0];
		read_only = true; // it is text segment, which should be read-only
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		// kprintf("data segment:\n");
		// kprintf("faultaddress - vbase2 is %d\n", (faultaddress - vbase2));
		// kprintf("faultaddress - vbase2 & PAGE_FRAME is %d\n", (faultaddress - vbase2) & PAGE_FRAME);
		// kprintf("as_pbase2[0] is %d\n", as->as_pbase2[0]);
		paddr = (faultaddress - vbase2) + as->as_pbase2[0];
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		// kprintf("stack segment:\n");
		// kprintf("faultaddress - stackbase is %d\n", (faultaddress - stackbase));
		// kprintf("faultaddress - stackbase & PAGE_FRAME is %d\n", (faultaddress - stackbase) & PAGE_FRAME);
		// kprintf("as_stackpbase[0] is %d\n", as->as_stackpbase[0]);
		paddr = (faultaddress - stackbase) + as->as_stackpbase[0];
	}
	else { 
		return EFAULT;
	}

	#else
	
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		kprintf("%d\n", as->as_pbase1);
		paddr = (faultaddress - vbase1) + as->as_pbase1;
		read_only = true; // it is text segment, which should be read-only
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		kprintf("%d\n", as->as_pbase2);
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		kprintf("%d\n", as->as_stackpbase);
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	#endif
	
	// kprintf("PAGE_FRAME is %d\n", PAGE_FRAME);
	// kprintf("paddr is %d\n", paddr);
	// kprintf("paddr & PAGE_FRAME is %d\n", paddr & PAGE_FRAME);

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	// kprintf("111111111111111111111111111\n");

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;

		#if OPT_A3
		bool load_elf = as->load_elf;
		if(read_only && load_elf) {
			elo &= ~TLBLO_DIRTY;
		}
		# endif
		



		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		
		return 0;
	}
	// kprintf("44444444444444444444444444444\n");
	
	#if OPT_A3
	bool load_elf = as->load_elf;
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	if(read_only && load_elf) {
		elo &= ~TLBLO_DIRTY;
	}
	tlb_random(ehi, elo);
    splx(spl);
	return 0;

	#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;

	#endif
	
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	#if OPT_A3
	as->as_vbase1 = 0;
	as->as_pbase1 = NULL;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = NULL;
	as->as_npages2 = 0;
	as->as_stackpbase = NULL;

	#else
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;
	#endif

	as->load_elf = false;

	return as;
}

void
as_destroy(struct addrspace *as)
{	
	#if OPT_A3
	for(size_t i = 0; i < as->as_npages1; i++) {
		free_kpages(PADDR_TO_KVADDR(as->as_pbase1[i]));
	}

	for(size_t i = 0; i < as->as_npages2; i++) {
		free_kpages(PADDR_TO_KVADDR(as->as_pbase2[i]));
	}

	for(size_t i = 0; i < DUMBVM_STACKPAGES; i++) {
		free_kpages(PADDR_TO_KVADDR(as->as_stackpbase[i]));
	}

	kfree(as->as_pbase1);
	kfree(as->as_pbase2);
	kfree(as->as_stackpbase);

	// kfree(as);
	#else
	kfree(as);
	#endif
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	#if OPT_A3
	// kprintf("this is my first time here\n");
	if (as->as_vbase1 == 0) {
		// kprintf("helllllllllllllllllllllllllllllllllllllllllllllo\n");
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		as->as_pbase1 = kmalloc(sizeof(paddr_t) * as->as_npages1);
		

		if(as->as_pbase1 == NULL) {
			return ENOMEM;
		}
		return 0;
	}

	if (as->as_vbase2 == 0) {
		// kprintf("he1111111111111111111111111111111111111111111111111111111\n");
		// kprintf("npages is %d\n", npages);
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		as->as_pbase2 = kmalloc(sizeof(paddr_t) * as->as_npages2);
		
		if(as->as_pbase2 == NULL) {
			return ENOMEM;
		}
		return 0;
	}
	#else
	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}
	#endif

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{

	#if OPT_A3
	
	for (unsigned int i = 0; i < as->as_npages1; i++) {
		
    	paddr_t physical_addr = getppages(1);
    	if (physical_addr == 0) {
			return ENOMEM;
		}
    	as->as_pbase1[i] = physical_addr;
    	as_zero_region(as->as_pbase1[i], 1);
  	}

	for (unsigned int i = 0; i < as->as_npages2; i++) {
		// kprintf("2. number for data\n");
		// kprintf("as_npages2 is %d\n", as->as_npages2);
    	paddr_t physical_addr = getppages(1);
    	if (physical_addr == 0) {
			return ENOMEM;
		}
    	as->as_pbase2[i] = physical_addr;
    	as_zero_region(as->as_pbase2[i], 1);
  	}

	as->as_stackpbase = kmalloc(sizeof(paddr_t) * DUMBVM_STACKPAGES);
	if(as->as_stackpbase == NULL) {
		return ENOMEM;
	}

	for (unsigned int i = 0; i < DUMBVM_STACKPAGES; i++) {
		
    	paddr_t physical_addr = getppages(1);
    	if (physical_addr == 0) {
			return ENOMEM;
		}
    	as->as_stackpbase[i] = physical_addr;
    	as_zero_region(as->as_stackpbase[i], 1);
  	}

	#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
	#endif

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	

	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	#if OPT_A3
	new->as_pbase1 = kmalloc(sizeof(paddr_t) * old->as_npages1);
    new->as_pbase2 = kmalloc(sizeof(paddr_t) * old->as_npages2);
    new->as_stackpbase = kmalloc(sizeof(paddr_t) * DUMBVM_STACKPAGES);

	
	#endif

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	#if OPT_A3
	KASSERT(new->as_pbase1 != NULL);
	KASSERT(new->as_pbase2 != NULL);
	KASSERT(new->as_stackpbase != NULL);
	// kprintf("hi\n");
	// kprintf("hi b4 memmove\n");

	int pages1 = new->as_npages1;
	int pages2 = new->as_npages2;
	int stack_page = DUMBVM_STACKPAGES;

	for (int i = 0; i < pages1; i++){
		KASSERT(new->as_pbase1[i] != 0);
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase1[i]), (const void *)PADDR_TO_KVADDR(old->as_pbase1[i]), PAGE_SIZE);
	}

	for (int i = 0; i < pages2; i++){
		KASSERT(new->as_pbase2[i] != 0);
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase2[i]), (const void *)PADDR_TO_KVADDR(old->as_pbase2[i]), PAGE_SIZE);
	}

	for (int i = 0; i < stack_page; i++){
		KASSERT(new->as_stackpbase[i] != 0);
		memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase[i]), (const void *)PADDR_TO_KVADDR(old->as_stackpbase[i]), PAGE_SIZE);
	}


	// memmove((void *)PADDR_TO_KVADDR(new->as_pbase1), (const void *)PADDR_TO_KVADDR(old->as_pbase1), old->as_npages1 * PAGE_SIZE);
	// memmove((void *)PADDR_TO_KVADDR(new->as_pbase2), (const void *)PADDR_TO_KVADDR(old->as_pbase2), old->as_npages2 * PAGE_SIZE);
	// memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase), (const void *)PADDR_TO_KVADDR(old->as_stackpbase), DUMBVM_STACKPAGES * PAGE_SIZE);
	// kprintf("hi after memmove\n");
	#else

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
	
	#endif
	*ret = new;
	
	return 0;
}
