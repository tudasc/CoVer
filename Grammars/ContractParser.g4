parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: (ContractMarker | ContractMarkerExpFail | ContractMarkerExpSucc) ScopePrefix precondition? postcondition? functags? ScopePostfix;

precondition: PreMarker ScopePrefix expression? ScopePostfix;
postcondition: PostMarker ScopePrefix expression? ScopePostfix;
functags: TagMarker ScopePrefix tagUnit (ListSep tagUnit)* ScopePostfix;

tagUnit: Variable (OPPrefix NatNum OPPostfix)?;

expression: primitive | composite;

primitive: readOp | writeOp | callOp;
readOp: OPRead OPPrefix Variable OPPostfix;
writeOp: OPWrite OPPrefix Variable OPPostfix;
varMap: (callP=NatNum | TagParam) MapSep contrP=NatNum;
callOp: (OPCall | OPCallTag) OPPrefix Variable (ListSep varMap)* OPPostfix;

composite: releaseOp;
releaseOp: OPRelease1 OPPrefix forbidden=primitive OPPostfix OPRelease2 OPPrefix until=callOp OPPostfix;
