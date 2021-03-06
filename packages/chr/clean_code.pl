/*  $Id$

    Part of CHR (Constraint Handling Rules)

    Author:        Tom Schrijvers
    E-mail:        Tom.Schrijvers@cs.kuleuven.be
    WWW:           http://www.swi-prolog.org
    Copyright (C): 2003-2004, K.U. Leuven

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    As a special exception, if you link this library with other files,
    compiled with a Free Software compiler, to produce an executable, this
    library does not by itself cause the resulting executable to be covered
    by the GNU General Public License. This exception does not however
    invalidate any other reasons why the executable file might be covered by
    the GNU General Public License.
*/

%   ____          _         ____ _                  _
%  / ___|___   __| | ___   / ___| | ___  __ _ _ __ (_)_ __   __ _
% | |   / _ \ / _` |/ _ \ | |   | |/ _ \/ _` | '_ \| | '_ \ / _` |
% | |__| (_) | (_| |  __/ | |___| |  __/ (_| | | | | | | | | (_| |
%  \____\___/ \__,_|\___|  \____|_|\___|\__,_|_| |_|_|_| |_|\__, |
%                                                           |___/
%
% To be done:
%	inline clauses

:- module(clean_code,
	[
		clean_clauses/2
	]).

:- use_module(library(dialect/hprolog)).

clean_clauses(Clauses,NClauses) :-
	clean_clauses1(Clauses,Clauses1),
	merge_clauses(Clauses1,NClauses).


%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
% CLEAN CLAUSES
%
%	- move neck unification into the head of the clause
%	- drop true body
%	- specialize control flow goal wrt true and fail
%
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

clean_clauses1([],[]).
clean_clauses1([C|Cs],[NC|NCs]) :-
	clean_clause(C,NC),
	clean_clauses1(Cs,NCs).

clean_clause(Clause,NClause) :-
	( Clause = (Head :- Body) ->
		clean_goal(Body,Body1),
		move_unification_into_head(Head,Body1,NHead,NBody),
		( NBody == true ->
			NClause = NHead
		;
			NClause = (NHead :- NBody)
		)
	; Clause = '$source_location'(File,Line) : ActualClause ->
		NClause = '$source_location'(File,Line) :  NActualClause,
		clean_clause(ActualClause,NActualClause)
	;
		NClause = Clause
	).

clean_goal(Goal,NGoal) :-
	var(Goal), !,
	NGoal = Goal.
clean_goal((G1,G2),NGoal) :-
	!,
	clean_goal(G1,NG1),
	clean_goal(G2,NG2),
	( NG1 == true ->
		NGoal = NG2
	; NG2 == true ->
		NGoal = NG1
	;
		NGoal = (NG1,NG2)
	).
clean_goal((If -> Then ; Else),NGoal) :-
	!,
	clean_goal(If,NIf),
	( NIf == true ->
		clean_goal(Then,NThen),
		NGoal = NThen
	; NIf == fail ->
		clean_goal(Else,NElse),
		NGoal = NElse
	;
		clean_goal(Then,NThen),
		clean_goal(Else,NElse),
		NGoal = (NIf -> NThen; NElse)
	).
clean_goal((G1 ; G2),NGoal) :-
	!,
	clean_goal(G1,NG1),
	clean_goal(G2,NG2),
	( NG1 == fail ->
		NGoal = NG2
	; NG2 == fail ->
		NGoal = NG1
	;
		NGoal = (NG1 ; NG2)
	).
clean_goal(once(G),NGoal) :-
	!,
	clean_goal(G,NG),
	( NG == true ->
		NGoal = true
	; NG == fail ->
		NGoal = fail
	;
		NGoal = once(NG)
	).
clean_goal((G1 -> G2),NGoal) :-
	!,
	clean_goal(G1,NG1),
	( NG1 == true ->
		clean_goal(G2,NGoal)
	; NG1 == fail ->
		NGoal = fail
	;
		clean_goal(G2,NG2),
		NGoal = (NG1 -> NG2)
	).
clean_goal(Goal,Goal).
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
move_unification_into_head(Head,Body,NHead,NBody) :-
	conj2list(Body,BodyList),
	move_unification_into_head_(BodyList,Head,NHead,NBody).

move_unification_into_head_([],Head,Head,true).
move_unification_into_head_([G|Gs],Head,NHead,NBody) :-
	( nonvar(G), G = (X = Y) ->
		term_variables(Gs,GsVars),
		( var(X), ( \+ memberchk_eq(X,GsVars) ; atomic(Y)) ->
			X = Y,
			move_unification_into_head_(Gs,Head,NHead,NBody)
		; var(Y), (\+ memberchk_eq(Y,GsVars) ; atomic(X)) ->
			X = Y,
			move_unification_into_head_(Gs,Head,NHead,NBody)
		;
			Head = NHead,
			list2conj([G|Gs],NBody)
		)
	;
		Head = NHead,
		list2conj([G|Gs],NBody)
	).


conj2list(Conj,L) :-				%% transform conjunctions to list
  conj2list(Conj,L,[]).

conj2list(G,L,T) :-
	var(G), !,
	L = [G|T].
conj2list(true,L,L) :- !.
conj2list(Conj,L,T) :-
  Conj = (G1,G2), !,
  conj2list(G1,L,T1),
  conj2list(G2,T1,T).
conj2list(G,[G | T],T).

list2conj([],true).
list2conj([G],X) :- !, X = G.
list2conj([G|Gs],C) :-
	( G == true ->				%% remove some redundant trues
		list2conj(Gs,C)
	;
		C = (G,R),
		list2conj(Gs,R)
	).

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
% MERGE CLAUSES
%
%	Find common prefixes of successive clauses and share them.
%
%	Note: we assume that the prefix does not generate a side effect.
%
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

merge_clauses([],[]).
merge_clauses([C],[C]).
merge_clauses([X,Y|Clauses],NClauses) :-
	( merge_two_clauses(X,Y,Clause) ->
		merge_clauses([Clause|Clauses],NClauses)
	;
		NClauses = [X|RClauses],
		merge_clauses([Y|Clauses],RClauses)
	).

merge_two_clauses('$source_location'(F1,L1) : C1,
		  '$source_location'(_F2,_L2) : C2,
		  Result) :- !,
	merge_two_clauses(C1,C2,C),
	Result = '$source_location'(F1,L1) : C.
merge_two_clauses((H1 :- B1), (H2 :- B2), (H :- B)) :-
	H1 =@= H2,
	H1 = H,
	conj2list(B1,List1),
	conj2list(B2,List2),
	merge_lists(List1,List2,H1,H2,Unifier,List,NList1,NList2),
	List \= [],
	H1 = H2,
	call(Unifier),
	list2conj(List,Prefix),
	list2conj(NList1,NB1),
	( NList2 == (!) ->
		B = Prefix
	;
		list2conj(NList2,NB2),
		B = (Prefix,(NB1 ; NB2))
	).

merge_lists([],[],_,_,true,[],[],[]).
merge_lists([],L2,_,_,true,[],[],L2).
merge_lists([!|Xs],_,_,_,true,[!|Xs],[],!) :- !.
merge_lists([X|Xs],[],_,_,true,[],[X|Xs],[]).
merge_lists([X|Xs],[Y|Ys],H1,H2,Unifier,Common,N1,N2) :-
	( H1-X =@= H2-Y ->
		Unifier = (X = Y, RUnifier),
		Common = [X|NCommon],
		merge_lists(Xs,Ys,H1/X,H2/Y,RUnifier,NCommon,N1,N2)
	;
		Unifier = true,
		Common = [],
		N1 = [X|Xs],
		N2 = [Y|Ys]
	).
