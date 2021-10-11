#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>


#include "opt-A2.h"
#include <mips/trapframe.h>
#include <synch.h>
#include <vfs.h>
#include <kern/fcntl.h>


#if OPT_A2
pid_t sys_fork(struct trapframe *tf, pid_t *retval)
{

  
  struct proc * child = proc_create_runprogram(curproc->p_name); // create a new process structure for the child process
  if(child == NULL) {
    return ECHILD;
  }
  if(child->pid == -1) { // then there is no more pid available and the child could not be created
    proc_destroy(child);

    return EBADF;
  }
  
  int err_code = as_copy(curproc_getas(), &(child->p_addrspace)); // Copy the address space from parent to child
  

  if(err_code != 0) {
    proc_destroy(child);
    
    return ENOMEM;
  }
  

  struct trapframe *child_tf;
  child_tf = kmalloc(sizeof(struct trapframe));
  if(child_tf == NULL) { // fail to creeate the trapeframe
    proc_destroy(child);
    
    return ENOMEM;
  }
  memcpy(child_tf, tf, sizeof(struct trapframe)); // copy the trapframe

  set_relationship(curproc, child);

  int thread_fork_res = thread_fork(curthread->t_name, child, enter_forked_process, (void *)child_tf, 0); // create a new thread
  if(thread_fork_res != 0) { // fail to create the thread
    proc_destroy(child);
    kfree(child_tf);
    
    return ENOMEM;

  }
  

  *retval = child->pid;

  

  return 0;




}
#endif /* OPT_A2 */





  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */


void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;


  #if OPT_A2
  p->exitcode = exitcode;
  p->is_exited = 1;
  lock_acquire(p->w_lock);


  int num_children = array_num(p->children);
  for(int i = num_children - 1; i >= 0; i--) {
    struct proc *kid = array_get(p->children, i);
    if(kid->is_exited == true) {
      kid->parent = NULL;
      array_remove(p->children, i);
      proc_destroy(kid);
    }
  }
  
  

  
    
  cv_signal(p->w_cv, p->w_lock);
  lock_release(p->w_lock);
  #endif
  

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  // kprintf("hi b4 sys_exit\n");
  as_destroy(as);
  // kprintf("hi after sys_exit\n");

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  
   if(p->parent == NULL) {
    proc_destroy(p);
   }
    
  
  
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");

}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */

  #if OPT_A2
  *retval = curproc->pid;
  #else
  *retval = 1;
  #endif /* OPT_A2 */
  
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;
  
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  #if OPT_A2
  int kid_index = find_index_by_pid(curproc, pid);

  // kprintf("kid_index is %d\n", kid_index);

  if(kid_index == -1) { // we did not find the kid
    // kprintf("we did not find the children\n\n");
    *retval = -1;
    return ECHILD;
  }

  struct proc *kid = array_get(curproc->children, kid_index);
  

  lock_acquire(kid->w_lock);
  while(kid->is_exited == 0) { // I am not leaving yet
    cv_wait(kid->w_cv, kid->w_lock);
  }
  lock_release(kid->w_lock);
  exitstatus = _MKWAIT_EXIT(kid->exitcode);

  
  
  #else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  #endif
  
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}


#if OPT_A2
int sys_execv(const char *program, char **args) {
  KASSERT(program != NULL);
  KASSERT(args != NULL);
  int num_args = 0;
  struct addrspace *as;
  struct vnode *v;
  vaddr_t entrypoint, stackptr;
  int result;

  int res_copy;
  
  int argslen = 0;
  char ** copy_args;
  size_t programlen = (strlen(program) + 1) * sizeof(char);


  // size_t programlen = 1024 * sizeof(char); // we got different result here

  // copy program name
  // int allocate_len = sizeof(char) * 128;

  
  char *progname = kmalloc(sizeof(char) * programlen);
  size_t *got;

  if(progname == NULL) {
    
    return ENOMEM;
  }
  
  
  res_copy = copyinstr((const userptr_t) program, progname, programlen, got);
  
  // kprintf("res_copy is %d\n", res_copy);
  // check if copy success
  if(res_copy != 0) { // we got a problem here
    
    return EFAULT;
  }
  
  

  // get the number of arguments and the total length of args
  while(args[num_args] != NULL) {

    argslen += strlen((char *)args[num_args]) + 1;
    num_args++;
  }

  // allocate space for copying args
  copy_args = kmalloc(sizeof(char *) * (num_args + 1));

  

  // copy each elements in args
  for(int i = 0; i < num_args; i++) {
    size_t s = (strlen((char *)(args[i])) + 1);
    copy_args[i] = kmalloc(sizeof(char) * s);
    res_copy = copyinstr((const userptr_t) args[i], (void *) copy_args[i], s, got);
    if(res_copy != 0) {
      return EINTR;
    }
  }
  
  copy_args[num_args] = NULL;


  /* Open the file. */
  int res_file = vfs_open(progname, O_RDONLY, 0, &v);

  if(res_file != 0) {
    return EACCES;
  }

  /* Create a new address space. */
  as = as_create();
  if(as == NULL) {
    vfs_close(v);
    
    return ENOMEM;
  }
  
  /* Switch to it and activate it. */
  struct addrspace *new_addrs = curproc_setas(as);
  as_activate();
  

  /* Load the executable. */
  result = load_elf(v, &entrypoint);
  if(result != 0) {
    vfs_close(v);
		return ENOEXEC;

  }

  /* Done with the file now. */
  vfs_close(v);


  /* Define the user stack in the address space */
  result = as_define_stack(as, &stackptr);

  if(result != 0) {
    return EADDRNOTAVAIL;
  }
  
  vaddr_t stack_ptr = stackptr;
  vaddr_t *stack_args = kmalloc(sizeof(vaddr_t) * (num_args + 1));
  
  stack_args[num_args] = (vaddr_t) NULL;
  
  
  for(int i = num_args - 1; i >= 0; i--) {
    
    stack_ptr = stack_ptr - (sizeof(char) * ROUNDUP(strlen(copy_args[i]) + 1, 4));
    copyoutstr((char *) copy_args[i], (userptr_t) stack_ptr, ROUNDUP(strlen(copy_args[i]) + 1, 4), got);
    stack_args[i] = stack_ptr;
  }
  
  

  for(int i = num_args; i >= 0; i--) {
    stack_ptr -= sizeof(vaddr_t);
    copyout((void *) &stack_args[i], (userptr_t) stack_ptr, sizeof(vaddr_t));
  }

  /* Warp to user mode. */
  vaddr_t n_stack_ptr = ROUNDUP(stack_ptr, 8);
  for(int i = 0; i < num_args; i++) {
    kfree(copy_args[i]);
  }
  kfree(progname);
  kfree(copy_args);
  kfree(stack_args);
  // kprintf("hi b4 new_addr\n");
  as_destroy(new_addrs);
  enter_new_process(num_args, (userptr_t) stack_ptr, n_stack_ptr, entrypoint);

  
  
  /* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

#endif
