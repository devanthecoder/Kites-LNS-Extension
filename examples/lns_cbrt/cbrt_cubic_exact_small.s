# TCP CUBIC cube-root benchmark; variant=cubic_exact; set=small; N=8
# Kernel: Linux v6.12 cubic_root() (https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c)
.data
input:
    .dword 0, 1, 2, 3
    .dword 4, 5, 6, 7
output: .zero 64
steps: .zero 32
vtab: .byte 0, 54, 54, 54, 118, 118, 118, 118, 123, 129, 134, 138, 143, 147, 151, 156, 157, 161, 164, 168, 170, 173, 176, 179, 181, 185, 187, 190, 192, 194, 197, 199, 200, 202, 204, 206, 209, 211, 213, 215, 217, 219, 221, 222, 224, 225, 227, 229, 231, 232, 234, 236, 237, 239, 240, 242, 244, 245, 246, 248, 250, 251, 252, 254
.text
main:
    la s0, input
    la s1, output
    la s2, steps
    li s3, 8
root_loop:
    ld s4, 0(s0)
    li s6, 0
    mv t1, s4
    li t0, 0
    beqz t1, fls_done
fls_loop:
    addi t0, t0, 1
    srli t1, t1, 1
    bnez t1, fls_loop
fls_done:
    sltiu t1, t0, 7
    bnez t1, cubic_direct
    li t1, 84
    mul t0, t0, t1
    srli t0, t0, 8
    addi t0, t0, -1
    slli t1, t0, 1
    add t1, t1, t0
    srl t2, s4, t1
    la t1, vtab
    add t1, t1, t2
    lbu t2, 0(t1)
    addi t2, t2, 10
    sll t2, t2, t0
    srli t2, t2, 6
    mv s5, t2
    j cubic_newton
cubic_direct:
    la t1, vtab
    add t1, t1, t0
    lbu t2, 0(t1)
    addi t2, t2, 35
    srli s5, t2, 6
    j cubic_done
cubic_newton:
    addi t1, s5, -1
    mul t2, s5, t1
    divu t3, s4, t2
    slli t4, s5, 1
    add t4, t4, t3
    li t5, 341
    mul t4, t4, t5
    srli s5, t4, 10
cubic_done:
    mv s7, s5                  # original approximate candidate
fix_down_check:
    beqz s5, fix_up_check
    divu t0, s4, s5
    mul t1, s5, s5
    bltu t0, t1, fix_down
    j fix_up_check
fix_down:
    addi s5, s5, -1
    addi s6, s6, -1
    j fix_down_check
fix_up_check:
    addi t2, s5, 1
    divu t0, s4, t2
    mul t1, t2, t2
    bltu t0, t1, fix_done
    mv s5, t2
    addi s6, s6, 1
    j fix_up_check
fix_done:
    sd s5, 0(s1)
    sw s6, 0(s2)
    addi s0, s0, 8
    addi s1, s1, 8
    addi s2, s2, 4
    addi s3, s3, -1
    bnez s3, root_loop
    li a0, 0
    li a7, 10
    ecall
