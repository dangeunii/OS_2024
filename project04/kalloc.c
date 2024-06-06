// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

#define PAGE_COUNT (PHYSTOP / PGSIZE) // 페이지 수
#define PAGE_INDEX(pa) ((pa) / PGSIZE)// 페이지 인덱스를 계산하는 매크로


void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
  int ref_cnt[PAGE_COUNT];
  int num_freePage;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE){
    kmem.ref_cnt[V2P(p)/PGSIZE] = 0;
    kfree(p);
  }
    
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  // -> 여기서 지우지 않고 그냥 다 free 해서 오류 발생
  //memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;

  //cnt_ref 줄이기
  if(kmem.ref_cnt[V2P(v)/PGSIZE] > 0) 
    kmem.ref_cnt[V2P(v)/PGSIZE] = kmem.ref_cnt[V2P(v)/PGSIZE] - 1;

  //ref_cnt==0 일 경우에만 메모리 free
  if(kmem.ref_cnt[V2P(v)/PGSIZE] == 0){   
    
    memset(v, 1, PGSIZE); 
    r->next = kmem.freelist;
    kmem.num_freePage = kmem.num_freePage + 1;
    kmem.freelist = r;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    // 처음 할당하는 순간 ref_cnt = 1
    kmem.ref_cnt[V2P((char*)r)/PGSIZE] = 1;
    // free page --
    kmem.num_freePage--;
    }

  if(kmem.use_lock){
    release(&kmem.lock);
  }
  return (char*)r;
}

// 메모리 참조값 증가
void 
incr_refc(uint pa)
{
  acquire(&kmem.lock);

  int index = PAGE_INDEX(pa);
  kmem.ref_cnt[index]++;

  release(&kmem.lock);

}

// 메모리 참조값 감소
void 
decr_refc(uint pa)
{
  acquire(&kmem.lock);
  int index = PAGE_INDEX(pa);
  kmem.ref_cnt[index]--;
  
  release(&kmem.lock);
}

// 메모리 참조 갯수 return
int 
get_refc(uint pa)
{
  int count;

  acquire(&kmem.lock);

  int index = PAGE_INDEX(pa);
  count = kmem.ref_cnt[index];
  
  release(&kmem.lock);

  return count;

} 

// free page 갯수 반환
int 
countfp(void)
{
  acquire(&kmem.lock);

  uint num_freePage = kmem.num_freePage;
  release(&kmem.lock);
  
  return num_freePage;
}
