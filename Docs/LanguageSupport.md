# Language Support

Currently, the following languages are supported:
- C/C++
- Fortran (1990, 2008(ts), 2018)

When analyzing Fortran code using predefined MPI contracts, choose the correct wrapper for your installation:
- `CoVerFC` assumes support for TS29113 by your MPI installation
- `CoVerFC-no-ts` can be used if your MPI installation has not yet been updated for TS29113.
