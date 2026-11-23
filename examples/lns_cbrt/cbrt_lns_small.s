# TCP CUBIC cube-root benchmark; variant=lns; set=small; N=8
# Kernel: Linux v6.12 cubic_root() (https://github.com/torvalds/linux/blob/v6.12/net/ipv4/tcp_cubic.c)
.data
input:
    .dword 0, 1, 2, 3
    .dword 4, 5, 6, 7
output: .zero 64
steps: .zero 32
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
    lpow l9, l8, l12
    fcvt.lu.lns s5, l9
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
