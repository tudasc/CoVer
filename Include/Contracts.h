#pragma once

#define CONTRACT(x) __attribute__((annotate("CONTRACT{" #x "}")))
#define CONTRACTXF(x) __attribute__((annotate("CONTRACTXFAIL{" #x "}")))
#define CONTRACTXS(x) __attribute__((annotate("CONTRACTXSUCC{" #x "}")))
