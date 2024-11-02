parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: (ContractMarker | ContractMarkerExpFail | ContractMarkerExpSucc) ScopePrefix precondition? postcondition? ScopePostfix;

precondition: PreMarker ScopePrefix expression? ScopePostfix;
postcondition: PostMarker ScopePrefix expression? ScopePostfix;

expression: primitive | composite;

primitive: readOp | writeOp | callOp;
readOp: OPRead OPPrefix Variable OPPostfix;
writeOp: OPWrite OPPrefix Variable OPPostfix;
callOp: OPCall OPPrefix Variable OPPostfix;

composite: releaseOp;
releaseOp: OPRelease1 OPPrefix forbidden=primitive OPPostfix OPRelease2 OPPrefix until=callOp OPPostfix;
