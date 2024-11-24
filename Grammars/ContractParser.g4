parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: (ContractMarker | ContractMarkerExpFail | ContractMarkerExpSucc) ScopePrefix precondition? postcondition? functags? ScopePostfix;

precondition: PreMarker ScopePrefix exprList ScopePostfix;
postcondition: PostMarker ScopePrefix exprList ScopePostfix;
exprList: exprFormula (ListSep exprFormula)*;
exprFormula: expression | (OPPrefix exprFormula (XORSep exprFormula)+ OPPostfix) | (OPPrefix exprFormula (ORSep exprFormula)+ OPPostfix);
functags: TagMarker ScopePrefix tagUnit (ListSep tagUnit)* ScopePostfix;

tagUnit: Variable (OPPrefix NatNum OPPostfix)?;

expression: primitive | composite;

primitive: readOp | writeOp | callOp;
readOp: OPRead OPPrefix (Deref | AddrOf)? NatNum OPPostfix;
writeOp: OPWrite OPPrefix (Deref | AddrOf)? NatNum OPPostfix;
varMap: (callP=NatNum | TagParam) MapSep (Deref | AddrOf)? contrP=NatNum;
callOp: (OPCall | OPCallTag) OPPrefix Variable (ListSep varMap)* OPPostfix;

composite: releaseOp;
releaseOp: OPRelease1 OPPrefix forbidden=primitive OPPostfix OPRelease2 OPPrefix until=callOp OPPostfix;
