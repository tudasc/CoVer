! This module defined Declare_Contract,
! which can then be used for the contract declarations by
! passing the function/subroutine as well as the contract string
module contract_helper
    interface
        subroutine Declare_Contract(funcPtr, contrString)
            procedure() :: funcPtr
            character(len=*), intent(in) :: contrString
        end subroutine
    end interface
end module
