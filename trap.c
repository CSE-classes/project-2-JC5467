#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;
extern int page_allocator_type;
int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  
  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(proc->killed)
      exit();
    proc->tf = tf;
    syscall();
    if(proc->killed)
      exit();
    return;
  }
  
 // CS 3320 project 2
 // You might need to change the folloiwng default page fault handling
 // for your project 2
   if(tf->trapno == T_PGFLT)
   {
    uint faulting_va = rcr2();

    // Only do special handling in LAZY allocator mode
    if(page_allocator_type == 1)
    {
      struct proc *curproc = myproc();

      // Make sure there is a current process and this is a user trap
      if(curproc && (tf->cs & 3) == DPL_USER)
      {
        uint page_va = PGROUNDDOWN(faulting_va);
        char *mem;

        // Valid lazy-heap access must be below the process size
        if(faulting_va < curproc->sz)
        {
          mem = kalloc();
          if(mem == 0)
          {
            // Out of memory
            cprintf("lazy allocation: out of memory\n");
            curproc->killed = 1;
          } else
           {
            // Clear the page and map it
            memset(mem, 0, PGSIZE);
            if(mappages(curproc->pgdir, (char*)page_va,
                        PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
              cprintf("lazy allocation: mappages failed\n");
              kfree(mem);
              curproc->killed = 1;
            } else {
              // Successfully handled the lazy page fault
              if(curproc->killed && (tf->cs & 3) == DPL_USER)
                exit();
              return;      // IMPORTANT: do not fall through to normal handler
            }
          }
        }
        // If faulting_va >= curproc->sz, it's an invalid access;
        // we just fall through and let the normal trap code kill the process.
      }
    }

    // If we get here, either:
    //  - we're not in lazy mode, or
    //  - the fault wasn't a valid lazy-heap access, or
    //  - allocation failed.
    // In all of those cases, we fall through and let the normal
    // xv6 trap handling decide what to do (usually kill the process).
  }

  // --------- KEEP THE REST OF YOUR ORIGINAL trap() HERE ---------
  //switch(tf->trapno) { ... timer/ide/keyboard/etc ... }
  //if(proc == 0 || (tf->cs&3) == 0) { panic(...) }
  // proc->killed = 1;
  // if(proc && proc->killed && (tf->cs&3) == DPL_USER) exit();
  // ...



  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpu->id == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpu->id, tf->cs, tf->eip);
    lapiceoi();
    break;
   
  //PAGEBREAK: 13
  default:
    if(proc == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpu->id, tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            proc->pid, proc->name, tf->trapno, tf->err, cpu->id, tf->eip, 
            rcr2());
    proc->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running 
  // until it gets to the regular system call return.)
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(proc && proc->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();
}
