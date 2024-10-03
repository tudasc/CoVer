parser grammar ContractParser;

options {
	tokenVocab = ContractLexer;
}

start: contract EOF;
contract: ContractPrefix + ScopePostfix;
