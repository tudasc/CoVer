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
NatNum: ('0' | [1-9] [0-9]*);

ListSep: ',';

// All ops must end with '!' to differentiate from variables
OPRead: 'read!';
OPWrite: 'write!';
OPCall: 'called!';
OPRelease1: 'no!';
OPRelease2: 'until!';
OPPrefix: '(';
OPPostfix: ')';
