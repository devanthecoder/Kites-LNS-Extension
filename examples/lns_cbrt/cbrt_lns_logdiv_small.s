# TCP CUBIC cube-root benchmark; variant=lns_logdiv; set=small; N=8
# Kernel: Linux v6.12 cubic_root() (https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c)
.data
input:
    .dword 0, 1, 2, 3
    .dword 4, 5, 6, 7
output: .zero 64
steps: .zero 32
scratch: .zero 4
.text
main:
    la s0, input
    la s1, output
    la s2, steps
    li s3, 8
    li t0, 1
    fcvt.lns.w l10, t0
    li t0, 3
    fcvt.lns.w l11, t0
    ldiv l12, l10, l11
root_loop:
    ld s4, 0(s0)
    li s6, 0
    fcvt.lns.lu l8, s4
    la t5, scratch
    lsw l8, 0(t5)
    lw t0, 0(t5)
    slli t0, t0, 1
    srai t0, t0, 1
    li t1, 3
    divw t0, t0, t1
    slli t0, t0, 1
    srli t0, t0, 1
    sw t0, 0(t5)
    llw l9, 0(t5)
    fcvt.lu.lns s5, l9
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
