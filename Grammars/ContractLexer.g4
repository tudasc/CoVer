lexer grammar ContractLexer;

WS: [ \t\r\n]+ -> skip;

ContractMarker: 'CONTRACT';
ContractMarkerExpFail: 'CONTRACTXFAIL';
ContractMarkerExpSucc: 'CONTRACTXSUCC';
PreMarker: 'PRE';
PostMarker: 'POST';
TagMarker: 'TAGS';
ScopePrefix: '{';
ScopePostfix: '}';

Variable: ([A-Z] | [a-z]) ([A-Z] | [a-z] | [0-9] | '_')*;
NatNum: ('0' | [1-9] [0-9]*);

ListSep: ',';

MapSep: ':';

TagParam: '$';

Deref: '*';
AddrOf: '&';

// All ops must end with '!' to differentiate from variables
OPRead: 'read!';
OPWrite: 'write!';
OPCall: 'called!';
OPCallTag: 'called_tag!';
OPRelease1: 'no!';
OPRelease2: 'until!';
OPPrefix: '(';
OPPostfix: ')';
