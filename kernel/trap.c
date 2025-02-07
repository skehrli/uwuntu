#include <cdefs.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>

// Interrupt descriptor table (shared by all CPUs).
struct gate_desc idt[256];
extern void *vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

int num_page_faults = 0;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    set_gate_desc(&idt[i], 0, SEG_KCODE << 3, vectors[i], KERNEL_PL);
  set_gate_desc(&idt[TRAP_SYSCALL], 1, SEG_KCODE << 3, vectors[TRAP_SYSCALL],
                USER_PL);

  initlock(&tickslock, "time");
}

void idtinit(void) { lidt((void *)idt, sizeof(idt)); }

void trap(struct trap_frame *tf) {
  uint64_t addr;

  if (tf->trapno == TRAP_SYSCALL) {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno) {
  case TRAP_IRQ0 + IRQ_TIMER:
    if (cpunum() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case TRAP_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + 7:
  case TRAP_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n", cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  default:
    addr = rcr2();

    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;

      // LAB3: page fault handling logic here

      // Handle Copy-on-write
      struct vregion *vr;
      struct vpage_info *vpi;
      if ((vr = va2vregion(&myproc()->vspace, addr)) != 0 && 
          (vpi = va2vpage_info(vr, addr)) != 0
      ) {

        struct core_map_entry* entry = (struct core_map_entry *)pa2page(vpi->ppn<<PT_SHIFT);

        if (vpi->cow_page && entry->ref_count > 1 && vpi->writable == 0) {
          // Copy-on-write page fault
          char* copy_page = kalloc();
          if (copy_page == 0) {
            panic("copy-on-write kalloc failed");
          }

          // Copy the page
          memset(copy_page, 0, PGSIZE);
          memmove(copy_page, P2V(vpi->ppn << PT_SHIFT), PGSIZE);

          // Update the page table entry
          acquire_core_map_lock();
          vpi->used = 1;
          vpi->ppn = PGNUM(V2P(copy_page));  // TODO: ???????? Lab3.md
          vpi->writable = VPI_WRITABLE;  // Make the page writable again
          vpi->present = VPI_PRESENT;    // Page is used in memory
          vpi->cow_page = 0;  // No longer a copy-on-write page

          entry->ref_count--; // Decrement the ref count because we copy the page
          release_core_map_lock();

          vspaceupdate(&myproc()->vspace);
          vspaceinstall(myproc());
          return;

        } else if (vpi->cow_page == true && entry->ref_count == 1 && vpi->writable == 0) { 
          // Only reference to unwritable page 
          acquire_core_map_lock();
          vpi->writable = VPI_WRITABLE; // Make it writable
          vpi->cow_page = 0;            // Make it not copy-on-write
          release_core_map_lock();

          vspaceupdate(&myproc()->vspace);
          vspaceinstall(myproc());
          return;
        }
      }

      // Handle Grow user stack
      if (SZ_2G - 10*PAGE_SIZE <= addr && addr < SZ_2G) {
        if (grow_ustack() >= 0) return;
      }

      if (myproc() == 0 || (tf->cs & 3) == 0) {
        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d rip %lx (cr2=0x%x)\n",
                tf->trapno, cpunum(), tf->rip, addr);
        panic("trap");
      }
    }

    // Assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == TRAP_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}

int grow_ustack(void) {
  struct vspace *vspace = &myproc()->vspace;
  struct vregion *ustack = &vspace->regions[VR_USTACK];

  if (ustack->size < 10*PAGE_SIZE) {
    uint64_t ustack_top = ustack->va_base - ustack->size - PGSIZE;  // stack grows downwards

    if (vregionaddmap(ustack, ustack_top, PAGE_SIZE, VPI_PRESENT, VPI_WRITABLE) >= 0) {
      ustack->size += PAGE_SIZE;
      vspaceupdate(vspace);
      return ustack_top;
    }
  }

  return -1;
}
