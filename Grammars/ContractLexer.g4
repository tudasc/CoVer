lexer grammar ContractLexer;

WS: [ \t\r\n]+ -> skip;

ContractMarker: 'CONTRACT';
ContractMarkerExpFail: 'CONTRACTXFAIL';
ContractMarkerExpSucc: 'CONTRACTXSUCC';
PreMarker: 'PRE';
PostMarker: 'POST';
ScopePrefix: '{';
ScopePostfix: '}';

Variable: ([A-Z] | [a-z]) ([A-Z] | [a-z] | [0-9] | '_')*;

// All ops must end with '!' to differentiate from variables
OPRead: 'read!';
OPWrite: 'write!';
OPPrefix: '(';
OPPostfix: ')';
