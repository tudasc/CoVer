parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: (ContractMarker | ContractMarkerExpFail | ContractMarkerExpSucc) ScopePrefix precondition? postcondition? functags? ScopePostfix;

precondition: PreMarker ScopePrefix exprList ScopePostfix;
postcondition: PostMarker ScopePrefix exprList ScopePostfix;
exprList: exprFormula (ListSep exprFormula)*;
exprFormula: (expression | (OPPrefix exprFormula (XORSep exprFormula)+ OPPostfix) | (OPPrefix exprFormula (ORSep exprFormula)+ OPPostfix)) (MsgMarker msg=String)?;
functags: TagMarker ScopePrefix tagUnit (ListSep tagUnit)* ScopePostfix;

tagUnit: Variable (OPPrefix NatNum OPPostfix)?;

expression: callOp | releaseOp | paramOp;

readOp: OPRead OPPrefix (Deref | AddrOf)? NatNum OPPostfix;
writeOp: OPWrite OPPrefix (Deref | AddrOf)? NatNum OPPostfix;
varMap: (callP=NatNum | TagParam) MapSep (Deref | AddrOf)? contrP=NatNum;
callOp: (OPCall | OPCallTag) OPPrefix Variable (ListSep varMap)* OPPostfix;
paramOp: OPParam OPPrefix NatNum MapSep paramReq (ListSep paramReq)* OPPostfix;
paramReq: (ParamEqExcept | ParamForbidEq | ParamGt | ParamGtEq | ParamLt | ParamLtEq) value=(Variable | NatNum);

relForbidden: readOp | writeOp | callOp;
releaseOp: OPRelease1 OPPrefix forbidden=relForbidden OPPostfix OPRelease2 OPPrefix until=callOp OPPostfix;
