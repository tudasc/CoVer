parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: (ContractMarker | ContractMarkerExpFail | ContractMarkerExpSucc) ScopePrefix precondition? postcondition? functags? ScopePostfix;

precondition: PreMarker ScopePrefix expression? ScopePostfix;
postcondition: PostMarker ScopePrefix expression? ScopePostfix;
functags: TagMarker ScopePrefix Variable (ListSep Variable)* ScopePostfix;

expression: primitive | composite;

primitive: readOp | writeOp | callOp;
readOp: OPRead OPPrefix Variable OPPostfix;
writeOp: OPWrite OPPrefix Variable OPPostfix;
callOp: (OPCall | OPCallTag) OPPrefix Variable (ListSep NatNum)* OPPostfix;

composite: releaseOp;
releaseOp: OPRelease1 OPPrefix forbidden=primitive OPPostfix OPRelease2 OPPrefix until=callOp OPPostfix;
