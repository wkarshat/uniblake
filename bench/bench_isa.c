/* Copyright (c) 2026 UniBlake Developers */
/* Per-instruction latency for the operations BLAKE2b's G function needs.
 *
 * Answers a question a kernel benchmark cannot: whether a vector kernel loses
 * because of how it was written, or because the instruction set charges more
 * per step than the scalar path does. Each loop is a serial dependency chain,
 * one operation per iteration, so the figure is latency rather than
 * throughput -- which is what matters on a critical path.
 *
 * Some loops without a data dependency are folded away by the optimiser and
 * report 0.000; that is expected and those rows are not the point.
 *
 * See backends/README.md, "NEON is slower than scalar here". */
#define _POSIX_C_SOURCE 200112L
#include <arm_neon.h>
#include <stdio.h>
#include <time.h>
static double ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
#define N 20000000
int main(void){
  uint64x2_t a=vdupq_n_u64(0x0123456789abcdefULL), b=vdupq_n_u64(1);
  const uint8x16_t idx={3,4,5,6,7,0,1,2, 11,12,13,14,15,8,9,10};
  volatile uint64_t sink=0; double t0;

  /* serial: each op depends on the last -> measures LATENCY */
  t0=ns(); for(int i=0;i<N;i++) a=vaddq_u64(a,b);
  double add_lat=(ns()-t0)/N; sink^=vgetq_lane_u64(a,0);

  t0=ns(); for(int i=0;i<N;i++) a=vreinterpretq_u64_u8(vqtbl1q_u8(vreinterpretq_u8_u64(a),idx));
  double tbl_lat=(ns()-t0)/N; sink^=vgetq_lane_u64(a,0);

  t0=ns(); for(int i=0;i<N;i++) a=vsriq_n_u64(vshlq_n_u64(a,1),a,63);
  double rot63_lat=(ns()-t0)/N; sink^=vgetq_lane_u64(a,0);

  t0=ns(); for(int i=0;i<N;i++) a=vextq_u64(a,a,1);
  double ext_lat=(ns()-t0)/N; sink^=vgetq_lane_u64(a,0);

  /* independent: 4 chains -> measures THROUGHPUT */
  uint64x2_t c0=a,c1=a,c2=a,c3=a;
  t0=ns(); for(int i=0;i<N;i++){c0=vaddq_u64(c0,b);c1=vaddq_u64(c1,b);c2=vaddq_u64(c2,b);c3=vaddq_u64(c3,b);}
  double add_tp=(ns()-t0)/N/4;
  sink^=vgetq_lane_u64(c0,0)^vgetq_lane_u64(c1,0)^vgetq_lane_u64(c2,0)^vgetq_lane_u64(c3,0);

  printf("NEON latency (serial chain), ns/op:\n");
  printf("  vadd.2d            %.3f\n", add_lat);
  printf("  tbl (rot24/16)     %.3f\n", tbl_lat);
  printf("  shl+sri (rot63)    %.3f\n", rot63_lat);
  printf("  ext (diagonalise)  %.3f\n", ext_lat);
  /* scalar, for comparison: same serial chain on 64-bit GPRs */
  uint64_t x=0x0123456789abcdefULL;
  t0=ns(); for(int i=0;i<N;i++) x+=1;
  double s_add=(ns()-t0)/N; sink^=x;
  t0=ns(); for(int i=0;i<N;i++) x=(x>>63)|(x<<1);
  double s_rot=(ns()-t0)/N; sink^=x;

  printf("NEON throughput (4 independent chains), ns/op:\n");
  printf("  vadd.2d            %.3f  -> %.1fx ILP\n", add_tp, add_lat/add_tp);
  printf("scalar latency, ns/op:\n");
  printf("  add                %.3f\n", s_add);
  printf("  ror                %.3f\n", s_rot);
  (void)sink; return 0;}
