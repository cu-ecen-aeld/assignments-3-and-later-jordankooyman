# Kernel Oops Analysis for /dev/faulty

## Trigger Command
```bash
echo "hello_world" > /dev/faulty
```

## Oops Output
```bash
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
  ESR = 0x0000000096000045
  EC = 0x25: DABT (current EL), IL = 32 bits
  SET = 0, FnV = 0
  EA = 0, S1PTW = 0
  FSC = 0x05: level 1 translation fault
Data abort info:
  ISV = 0, ISS = 0x00000045
  CM = 0, WnR = 1
user pgtable: 4k pages, 39-bit VAs, pgdp=0000000041bd7000
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
Internal error: Oops: 0000000096000045 [#1] SMP
Modules linked in: scull(O) faulty(O) hello(O)
CPU: 0 PID: 127 Comm: sh Tainted: G           O       6.1.44 #1
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x10/0x20 [faulty]
lr : vfs_write+0xc8/0x390
sp : ffffffc008dabd20
x29: ffffffc008dabd80 x28: ffffff8001c34f80 x27: 0000000000000000
x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
x23: 000000000000000c x22: 000000000000000c x21: ffffffc008dabdc0
x20: 000000555979b990 x19: ffffff8001bf8c00 x18: 0000000000000000
x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
x11: 0000000000000000 x10: 0000000000000000 x9 : 0000000000000000
x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
x5 : 0000000000000001 x4 : ffffffc000785000 x3 : ffffffc008dabdc0
x2 : 000000000000000c x1 : 0000000000000000 x0 : 0000000000000000
Call trace:
 faulty_write+0x10/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x1c/0x30
 invoke_syscall+0x54/0x130
 el0_svc_common.constprop.0+0x44/0xf0
 do_el0_svc+0x2c/0xc0
 el0_svc+0x2c/0x90
 el0t_64_sync_handler+0xf4/0x120
 el0t_64_sync+0x18c/0x190
Code: d2800001 d2800000 d503233f d50323bf (b900003f) 
---[ end trace 0000000000000000 ]---
```

## Analysis
The faulty device has a null pointer dereference issue, clearly stated and shown with the "kernel NULL pointer dereference at virtual address 0000000000000000" in line 1 of the oops log.
The log also gives a call trace and register dump, where both clearly show that the issue was caused by the "faulty_write" function.
In the register dump it can be seen that x0 and x1 are both 0, and the code disassembly shown at the end can be undone to show that "b900003f" is the "str wzr, [x1]" instruction ( using [generative AI](https://chat.deepseek.com/share/4xbi21nytt92zdpu2l) ), where it is trying to store a value at the address stored in x1, a NULL pointer. This then tells us what exactly went wrong, and the other code could be used to identify where in the source code the issue occured to then resolve it.

This error was small enough that it did not cause the kernel to fully crash, but it is likely to leave it unstable. Other errors could cause the kernel to crash more severely and may not recover as gracefully (here it stumbled, output the oops message, then reloaded the shell terminal). These types of bugs are unacceptable in production-level kernel code and must be resolved, and parsing the output logs is a big step towards accomplishing such a task.
