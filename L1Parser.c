#include "L1Parser.h"
#include <stdlib.h>
#include "L1Region.h"
#include <iso646.h>
#include "L1Array.h"
#include "L1Lexer.h"
#include <stdbool.h>

struct L1Parser
{
	L1ParserASTNode* rootASTNode;
	L1Region* region;
};

static L1ParserASTNode* ParserASTNodeFromToken(L1Parser* self, const L1ParserLexedToken* token)
{
	L1ParserASTNode* node = NULL;
	switch (token->type)
	{
		case L1LexerTokenTypeNatural:
			node = L1RegionAllocate(self->region, sizeof(L1ParserASTNode));
			node->type = L1ParserASTNodeTypeNatural;
			node->data.natural.bytes = token->bytes;
			node->data.natural.byteCount = token->byteCount;
			break;
		case L1LexerTokenTypeString:
			node = L1RegionAllocate(self->region, sizeof(L1ParserASTNode));
			node->type = L1ParserASTNodeTypeString;
			node->data.string.bytes = token->bytes;
			node->data.string.byteCount = token->byteCount;
			break;
		case L1LexerTokenTypeIdentifier:
			node = L1RegionAllocate(self->region, sizeof(L1ParserASTNode));
			node->type = L1ParserASTNodeTypeIdentifier;
			node->data.identifier.bytes = token->bytes;
			node->data.identifier.byteCount = token->byteCount;
			break;
		default:
			node = NULL;
			break;
	}
	return node;
}

/*
program = openexpression done

openexpression = branch
openexpression = assignment
openexpression = chainedexpression

branch = chainedexpression questionmark chainedexpression terminal openexpression

assignment = closedexpression assignment_arguments assign chainedexpression terminal openexpression
assignment_arguments = assignment_target assignment_arguments
assignment_arguments = .
assignment_target = identifier
assignment_target = openingsquarebracket assignment_target_list_body closingsquarebracket
assignment_target_list_body = assignment_target comma assignment_target_list_body
assignment_target_list_body = assignment_target comma
assignment_target_list_body = assignment_target
assignment_arguments = closedexpression

chainedexpression = closedexpression chainedexpression
chainedexpression = closedexpression

closedexpression = identifier
closedexpression = natural
closedexpression = string
closedexpression = openingparenthesis openexpression closingparenthesis
*/

typedef struct Rule Rule;
struct Rule
{
	uint8_t* symbols;
	uint8_t
		symbol,
		symbolCount,
		action;
}

static void* HandleAction(L1Parser* self, void* matchedSymbolData[], Rule rule);

static uint64_t Parse(L1Parser* self, const L1ParserLexedToken* tokens, uint64_t tokenCount, void** data, uint8_t currentNonterminalSymbol, const Rule* rules, uint64_t ruleCount)
{
	*data = NULL;
	for (uint64_t i = 0; i < tokenCount; i++)
	{
		const Rule rule = rules[i];
		if (rule.symbol == currentNonterminalSymbol)
		{
			void* matchedSymbolData[rule.symbolCount];
			bool matched = true;
			uint64_t currentTokenIndex = 0;
			for (uint8_t j = 0; j < rule.symbolCount; j++)
			{
				matchedSymbolData[j] = NULL;
				uint8_t symbol = rule.symbols[j];
				if(currentTokenIndex >= tokenCount)
				{
					matched = false;
					break;
				}
				if(tokens[currentTokenIndex].type == symbol)
				{
					matchedSymbolData[j] = ParserASTNodeFromToken(self, tokens + currentTokenIndex);
					currentTokenIndex++;
				}
				else
				{
					bool symbolIsRule = false;
					for (uint64_t k = 0; k < ruleCount; k++)
					{
						if(symbol == rules[k].symbol)
						{
							symbolIsRule = true;
							break;
						}
					}
					if(symbolIsRule)
					{
						uint64_t tokensRead = Parse(self, tokens + currentTokenIndex, tokenCount - currentTokenIndex, matchedSymbolData + j, symbol, rules, ruleCount);
						if(not tokensRead)
						{
							matched = false;
							break;
						}
						currentTokenIndex += tokensRead;
					}
					else
					{
						matched = false;
						break;
					}
				}
			}
			if(matched)
			{
				*data = HandleAction(self, matchedSymbolData, rule);
				return currentTokenIndex;
			}
		}
	}
}

L1Parser* L1ParserNew(const L1ParserLexedToken* tokens, uint64_t tokenCount)
{
	L1Parser* self = calloc(1, sizeof(L1Parser));
	self->region = L1RegionNew();
	uint64_t readCount = 0;
	void* rootASTNode = NULL;
	if(not Parse(self, tokens, tokenCount, & rootASTNode))
	{
		
	}
	self->rootASTNode = rootASTNode;
	return self;
}

const L1ParserASTNode* L1ParserGetRootASTNode(L1Parser* self)
{
	return self->rootASTNode;
}

void L1ParserDelete(L1Parser* self)
{
	if (not self) return;
	L1RegionDelete(self->region);
	free(self);
}