parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: (ContractMarker | ContractMarkerExpFail | ContractMarkerExpSucc) ScopePrefix precondition? postcondition? ScopePostfix;

precondition: PreMarker ScopePrefix expression? ScopePostfix;
postcondition: PostMarker ScopePrefix expression? ScopePostfix;

expression: primitive;

primitive: readOp | writeOp | callOp;
readOp: OPRead OPPrefix Variable OPPostfix;
writeOp: OPWrite OPPrefix Variable OPPostfix;
callOp: OPCall OPPrefix Variable OPPostfix;
