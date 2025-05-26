lexer grammar ContractLexer;

WS: [ \t\r\n]+ -> skip;

ContractMarker: 'CONTRACT';
ContractMarkerExpFail: 'CONTRACTXFAIL';
ContractMarkerExpSucc: 'CONTRACTXSUCC';
PreMarker: 'PRE';
PostMarker: 'POST';
TagMarker: 'TAGS';
MsgMarker: 'MSG';
ScopePrefix: '{';
ScopePostfix: '}';

String: '"' ([A-Z] | [a-z] | ' ' | '_' | '-' | '!' | '?' | ',' | [0-9])+ '"';
Variable: ([A-Z] | [a-z]) ([A-Z] | [a-z] | [0-9] | '_')*;
NatNum: ('0' | [1-9] [0-9]*);

ListSep: ',';
XORSep: '^';
ORSep: '|';

MapSep: ':';

TagParam: '$';

Deref: '*';
AddrOf: '&';

// All ops must end with '!' to differentiate from variables
OPRead: 'read!';
OPWrite: 'write!';
OPCall: 'call!';
OPCallTag: 'call_tag!';
OPRelease1: 'no!';
OPRelease2: 'until!';
OPPrefix: '(';
OPPostfix: ')';
