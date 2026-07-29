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

expression: callOp | releaseOp | paramOp | rwOp; // rwOp only makes sense for alloc though

natExpr: NatNum MarkArg?;
multExpr: Deref mathExpr;
mathOp: multExpr;
mathExpr: natExpr mathOp?;

rwOp: (OPRead | OPWrite | OPAlloc | OPFree) OPPrefix (Deref | AddrOf)? arg_index=(NatNum | RetSym) (RWOffsetPrefix alloc_size=mathExpr RWOffsetSuffix)? OPPostfix;
varMap: (callP=NatNum | TagParam) MapSep (Deref | AddrOf)? contrP=NatNum;
callOp: (OPCall | OPCallTag) OPPrefix Variable (ListSep varMap)* OPPostfix;
paramOp: OPParam OPPrefix NatNum MapSep paramReq (ListSep paramReq)* OPPostfix;
paramReq: (ParamEqExcept | ParamEq | ParamForbidEq | ParamGt | ParamGtEq | ParamLt | ParamLtEq) (value=Variable | (value=NatNum MarkArg?));

relForbidden: rwOp | callOp;
releaseOp: OPRelease1 OPPrefix forbidden=relForbidden OPPostfix OPRelease2 OPPrefix until=callOp OPPostfix;
