lexer grammar ContractLexer;

WS: [ \t\r\n]+ -> skip;

ContractPrefix: 'CONTRACT{';
ScopePostfix: '}';
