/* Darwin/PPC dyld hands off to an entry point with argc at the stack base. */
.text
.align 2
.globl start
start:
    lwz r3,0(r1)
    addi r4,r1,4
    li r0,0
    stwu r1,-64(r1)
    stw r0,0(r1)
    bl _main
    bl _exit
    trap
