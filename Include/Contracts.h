#pragma once

#define CONTRACT(...) __attribute__((annotate("CONTRACT{" #__VA_ARGS__ "}")))
#define CONTRACTXF(...) __attribute__((annotate("CONTRACTXFAIL{" #__VA_ARGS__ "}")))
#define CONTRACTXS(...) __attribute__((annotate("CONTRACTXSUCC{" #__VA_ARGS__ "}")))
