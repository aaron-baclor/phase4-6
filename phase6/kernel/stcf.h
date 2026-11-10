#ifndef XV6_STCF_H
#define XV6_STCF_H

// starting CPU budget
#define DEFAULT_STCF_BURST 100

// for phase 3
#define STCF_AT 20
#define STCF_AD 20

// Phase 5, Section 4.1.5: dynamic membership uses eight groups.
#define N_GROUPS 8
#define DEFAULT_SHARES 100
// Phase 6, Section 4.1.6: dynamic shares boosting.
#define SHARES_BOOSTING_RATIO 2
#define BOOSTED_SHARES 200
#define SHARES_BOOSTING_INTERVAL 10

// Section 2.5: retain fractional runtime even when shares exceed 1024.
#define VRUNTIME_SCALE (1ULL << 32)
#endif
