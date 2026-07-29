! This module defines helper functions for contract declarations for CoVer
module contract_helper
    interface
        ! Declare_Contract allows the definition of contracts.
        ! First argument must be the API function to apply the contract to.
        ! Second argument must be the contract string literal
        subroutine Declare_Contract(funcPtr, contrString)
            procedure() :: funcPtr
            character(len=*), intent(in) :: contrString
        end subroutine
        ! Declare_Value allows exposing constant values to contracts for parameter checking.
        ! First argument must be a string literal and will be the name used in the relevant contracts
        ! Second argument is the value itself.
        subroutine Declare_Value(name, value)
            character(len=*), intent(in) :: name
            class(*), optional, intent(in) :: value(..)
        end subroutine
    end interface
end module
