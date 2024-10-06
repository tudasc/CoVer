parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: ContractMarker ScopePrefix precondition? postcondition? ScopePostfix;

precondition: PreMarker ScopePrefix expression? ScopePostfix;
postcondition: PostMarker ScopePrefix expression? ScopePostfix;

expression: primitive;

primitive: OPRead OPPrefix Variable OPPostfix;
