#pragma once
#include "Contracts.h"

int MPI_Finalize(void) CONTRACT(PRE {called!(MPI_Init)});
