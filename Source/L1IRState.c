#include "L1IRState.h"
#include <iso646.h>

#include "L1IRSlotDescriptions"
#include "L1IRSlotAccessors"

static L1IRSlotType SlotTypeForBlock(L1IRGlobalStateBlockType type)
{
	switch (type)
	{
		case L1IRGlobalStateBlockTypeForeignFunction:
			return L1IRSlotTypeLambda;
		case L1IRGlobalStateBlockTypeLambda:
			return L1IRSlotTypeLambda;
		case L1IRGlobalStateBlockTypePi:
			return L1IRSlotTypePi;
		case L1IRGlobalStateBlockTypeSigma:
			return L1IRSlotTypeSigma;
		case L1IRGlobalStateBlockTypeADT:
			return L1IRSlotTypeADT;
		default:
			break;
	}
	abort();
}

///State Boilerplate

void L1IRLocalStateInitialize(L1IRLocalState* self)
{
	L1ArrayInitialize(& self->slots);
	L1ArrayInitialize(& self->gcBarriers);
	self->callDepth = 0;
}

void L1IRLocalStateDeinitialize(L1IRLocalState* self)
{
	L1ArrayDeinitialize(& self->slots);
	L1ArrayDeinitialize(& self->gcBarriers);
	self->callDepth = 0;
}

L1IRLocalAddress L1IRLocalStateCreateSlot(L1IRLocalState* self, L1IRSlot slot)
{
	L1ArrayPush(& self->slots, & slot, sizeof(L1IRSlot));
	return L1ArrayGetElementCount(& self->slots) - 1;
}

void L1IRGlobalStateInitialize(L1IRGlobalState* self)
{
	L1ArrayInitialize(& self->blocks);
}

void L1IRGlobalStateDeinitialize(L1IRGlobalState* self)
{
	const L1IRGlobalStateBlock* blocks = L1ArrayGetElements(& self->blocks);
	size_t blockCount = L1ArrayGetElementCount(& self->blocks);

	for (size_t i = 0; i < blockCount; i++)
	{
		if (L1IRGlobalStateBlockIsNative(blocks + i))
			free(blocks[i].native.slots);
	}
	L1ArrayDeinitialize(& self->blocks);
}

//Garbage Collection / Normalization / Deadcode Eliminator / Stack Compaction

static uint16_t CompactLocalGarbage(L1IRSlot* slots, uint16_t slotStart, uint16_t slotCount, uint16_t* roots, size_t rootCount)
{
	//if (rootCount == 0 or slotCount == slotStart) return slotStart;
	//assert (slotCount > slotStart);
	uint16_t maxUsedSlotCount = slotStart;
	//Mark roots
	for (size_t i = 0; i < rootCount; i++)
	{
		assert (roots[i] < slotCount);
		if (roots[i] + 1 > maxUsedSlotCount) maxUsedSlotCount = roots[i] + 1;
		L1IRSetSlotAnnotation(slots + roots[i], 1);
	}

	if (maxUsedSlotCount == slotStart) return slotStart;

	uint16_t finalSlotCount = slotStart;

	//Propagate retain marks
	for (uint16_t i = maxUsedSlotCount; i-- > slotStart;)
	{
		L1IRSlot slot = slots[i];
		L1IRSlotType type = L1IRExtractSlotType(slot);
		if (not IsImplicitRoot(type) and not L1IRExtractSlotAnnotation(slot)) continue;
		finalSlotCount++;
		for (uint8_t j = 0; j < 3; j++)
		{
			if (not SlotTypeArgumentIsLocalAddress(type, j)) continue;
			uint16_t operand = L1IRExtractSlotOperand(slot, j);
			if (operand < slotStart) continue;
			L1IRSetSlotAnnotation(slots + operand, 1);
		}
	}

	uint16_t* slotRemappings = malloc(sizeof(uint16_t) * (maxUsedSlotCount - slotStart));

	uint16_t finalSlotIndex = slotStart;
	
	//Compact reachable memory and fix references
	for (uint16_t i = slotStart; i < maxUsedSlotCount; i++)
	{
		L1IRSlot slot = slots[i];
		slotRemappings[i - slotStart] = UINT16_MAX;
		L1IRSlotType type = L1IRExtractSlotType(slot);
		if (not IsImplicitRoot(type) and not L1IRExtractSlotAnnotation(slot)) continue;
		uint16_t operands[3] = {0, 0, 0};
		for (uint8_t j = 0; j < 3; j++)
		{
			operands[j] = L1IRExtractSlotOperand(slot, j);
			if (not SlotTypeArgumentIsLocalAddress(type, j)) continue;
			operands[j] = slotRemappings[operands[j] - slotStart];
		}
		slotRemappings[i - slotStart] = finalSlotIndex;
		slots[finalSlotIndex] = L1IRMakeSlot(type, operands[0], operands[1], operands[2]);
		finalSlotIndex++;
	}
	
	//Update root handles
	for (size_t i = 0; i < rootCount; i++)
	{
		roots[i] = slotRemappings[roots[i] - slotStart];
	}
	
	free(slotRemappings);

	return finalSlotCount;
}

void L1IRGlobalStatePushGCBarrier(L1IRGlobalState* self, L1IRLocalState* localState)
{
	size_t slotCount = L1ArrayGetElementCount(& localState->slots);
	
	L1ArrayPush(& localState->gcBarriers, & slotCount, sizeof(size_t));
}

void L1IRGlobalStatePopGCBarrier(L1IRGlobalState* self, L1IRLocalState* localState, uint16_t* roots, size_t rootCount)
{
	size_t barrierSlotCount = 0;
	L1ArrayPop(& localState->gcBarriers, & barrierSlotCount, sizeof(size_t));
	size_t oldSlotCount = L1ArrayGetElementCount(& localState->slots);
	barrierSlotCount = CompactLocalGarbage(L1ArrayGetElements(& localState->slots), barrierSlotCount, oldSlotCount, roots, rootCount);
	L1ArraySetElementCount(& localState->slots, barrierSlotCount, sizeof(L1IRSlot));
}

//Dependency checking

static bool SlotDependsOnSlot(L1IRGlobalState* self, L1IRLocalState* localState, uint16_t dependentLocalAddress, uint16_t dependencyLocalAddress)
{
	if (dependencyLocalAddress == dependentLocalAddress) return true;
	if (dependencyLocalAddress > dependentLocalAddress) return false;
	const L1IRSlot* slots = L1ArrayGetElements(& localState->slots);
	L1IRSlot dependentSlot = slots[dependentLocalAddress];
	L1IRSlotType type = L1IRExtractSlotType(dependentSlot);
	for (uint8_t i = 0; i < 3; i++)
		if (SlotTypeArgumentIsLocalAddress(type, i))
			if (SlotDependsOnSlot(self, localState, L1IRExtractSlotOperand(dependentSlot, i), dependencyLocalAddress))
				return true;
	return false;
}

//Block Creation

L1IRGlobalAddress L1IRGlobalStateCreateNativeBlock(L1IRGlobalState* self, L1IRGlobalStateBlockType type, const L1IRSlot* slots, uint16_t slotCount, L1IRLocalAddress argumentLocalAddress)
{
	assert(type not_eq L1IRGlobalStateBlockTypeForeignFunction);
	
	const L1IRSlot* normalizedSlots = slots;
	uint16_t normalizedSlotCount = slotCount;
	const L1IRGlobalStateBlock* blocks = L1ArrayGetElements(& self->blocks);
	size_t blockCount = L1ArrayGetElementCount(& self->blocks);
	assert (normalizedSlotCount > 0);
	assert (slotCount > 0);

	L1IRGlobalAddress address = 0;
	for (size_t i = 0; i < blockCount; i++)
	{
		if (type == L1IRGlobalStateBlockTypeADT) break;
		if (blocks[i].type not_eq type) continue;
		if (blocks[i].native.slotCount not_eq normalizedSlotCount) continue;
		if (memcmp(blocks->native.slots, normalizedSlots, sizeof(L1IRSlot) * normalizedSlotCount) not_eq 0) continue;
		address = i;
		return address;
	}

	L1IRGlobalStateBlock block;
	block.type = type;
	block.native.slotCount = normalizedSlotCount;
	block.native.slots = memcpy(malloc(sizeof(L1IRSlot) * normalizedSlotCount), normalizedSlots, sizeof(L1IRSlot) * normalizedSlotCount);

	L1ArrayAppend(& self->blocks, & block, sizeof(L1IRGlobalStateBlock));
	address = blockCount;

	return address;
}

L1IRGlobalAddress L1IRGlobalStateCreateForeignBlock(L1IRGlobalState* self, L1IRGlobalStateBlockType type, L1IRGlobalStateBlockCallback callback, void* userdata)
{
	assert(type == L1IRGlobalStateBlockTypeForeignFunction);
	
	const L1IRGlobalStateBlock* blocks = L1ArrayGetElements(& self->blocks);
	size_t blockCount = L1ArrayGetElementCount(& self->blocks);

	L1IRGlobalAddress address = 0;

	L1IRGlobalStateBlock block;
	block.type = type;
	block.foreign.callback = callback;
	block.foreign.userdata = userdata;

	L1ArrayAppend(& self->blocks, & block, sizeof(L1IRGlobalStateBlock));
	address = blockCount;

	return address;
}

static L1IRLocalAddress WalkCaptureChain(const L1IRSlot* slots, L1IRLocalAddress captureLocalAddress, size_t depth)
{
	L1IRSlot captureSlot = slots[captureLocalAddress];
	for(uint16_t i = 0; i < depth; i++)
	{
		captureSlot = slots[CallCapture_captures(captureSlot)];
	}
	return CallCapture_captured(captureSlot);
}

L1IRLocalAddress L1IRGlobalStateEvaluate(L1IRGlobalState* self, L1IRLocalState* localState, L1IRGlobalStateEvaluationFlags flags, L1IRGlobalAddress calleeAddress, L1IRLocalAddress argumentLocalAddress, L1IRLocalAddress captureLocalAddress, L1IRLocalAddress* finalArgumentLocalAddressOut)
{
	L1IRLocalAddress resultLocalAddress = 0;
	L1IRLocalAddress finalArgumentLocalAddress = 0;

	assert (L1ArrayGetElementCount(& self->blocks) > calleeAddress);
	const L1IRGlobalStateBlock* block = calleeAddress + (const L1IRGlobalStateBlock*) L1ArrayGetElements(& self->blocks);
	
	localState->callDepth++;
	
	L1IRGlobalStatePushGCBarrier(self, localState);
	
	if (not L1IRGlobalStateBlockIsNative(block))
	{
		assert(flags & L1IRGlobalStateEvaluationFlagHasArgument);
		assert(not (flags & L1IRGlobalStateEvaluationFlagHasCaptured));
		finalArgumentLocalAddress = block->foreign.callback(self, localState, calleeAddress, argumentLocalAddress, finalArgumentLocalAddressOut, block->foreign.userdata);
		goto resultComputed;
	}
	
	assert (block->native.slotCount > 0);
	assert (block->native.slots);
	uint16_t* mergingSlotRemappings = malloc(sizeof(uint16_t) * block->native.slotCount);
	
	for (uint16_t i = 0; i < block->native.slotCount; i++)
	{
		const L1IRSlot prototypeSlot = block->native.slots[i];
		L1IRSlotType type = L1IRExtractSlotType(prototypeSlot);
		uint16_t operands[3] = {0, 0, 0};
		for (uint8_t j = 0; j < 3; j++) operands[j] = L1IRExtractSlotOperand(prototypeSlot, j);
		for (uint8_t j = 0; j < 3; j++) operands[j] = SlotTypeArgumentIsLocalAddress(type, j) ? mergingSlotRemappings[operands[j]]: operands[j];
		/*for (uint8_t j = 0; j < 3; j++)
		{
			if (not SlotTypeArgumentIsLocalAddress(type, j))
				break;
			const L1IRSlot* slots = L1ArrayGetElements(& localState->slots);
			L1IRSlot errorSlot = slots[operands[j]];
			if (L1IRExtractSlotType(errorSlot) not_eq L1IRSlotTypeError)
				break;
			goto done;
		}*/
		switch (type)
		{
			case L1IRSlotTypeUnresolvedSymbol:
			case L1IRSlotTypeError:
				{
					mergingSlotRemappings[i] = L1IRGlobalStateCreateSlot(self, localState, L1IRMakeSlot(L1IRSlotTypeError, L1IRErrorTypeInvalidInstruction, 0, 0));
					goto finish;
				}
			case L1IRSlotTypeArgument:
				assert(operands[0] == 0);
				if (not (flags & L1IRGlobalStateEvaluationFlagHasArgument))
					mergingSlotRemappings[i] = L1IRGlobalStateCreateSlot(self, localState, L1IRMakeSlot(L1IRSlotTypeArgument, localState->callDepth - 1, operands[1], 0));
				else
					mergingSlotRemappings[i] = argumentLocalAddress;
				
				if (finalArgumentLocalAddressOut)
					finalArgumentLocalAddress = mergingSlotRemappings[i];
				
				if (not L1IRGlobalStateIsOfType(self, localState, mergingSlotRemappings[i], operands[1]))
				{
					mergingSlotRemappings[i] = L1IRGlobalStateCreateSlot(self, localState, L1IRMakeSlot(L1IRSlotTypeError, L1IRErrorTypeTypeChecking, 0, 0));
					goto finish;
					//abort();
				}
				break;
			case L1IRSlotTypeCaptured:
				if (flags & L1IRGlobalStateEvaluationFlagHasCaptured)
					mergingSlotRemappings[i] = WalkCaptureChain(L1ArrayGetElements(& localState->slots), captureLocalAddress, operands[0]);
				else
					mergingSlotRemappings[i] = L1IRGlobalStateCreateSlot(self, localState, L1IRMakeSlot(L1IRSlotTypeCaptured, operands[0], 0, 0));
				break;
			case L1IRSlotTypeSelf:
				if (localState->callDepth > 1)
				{
					uint16_t captureLocalAddress = 0;

					if (flags & L1IRGlobalStateEvaluationFlagHasCaptured)
						captureLocalAddress = WalkCaptureChain(L1ArrayGetElements(& localState->slots), captureLocalAddress, operands[0]);
					else
						captureLocalAddress = L1IRGlobalStateCreateSlot(self, localState, L1IRMakeSlot(L1IRSlotTypeCaptured, 0, 0, 0));

					mergingSlotRemappings[i] = L1IRGlobalStateCreateSlot(self, localState, L1IRMakeSlot(SlotTypeForBlock(block->type), captureLocalAddress, calleeAddress, 0));
					break;
				}
			default:
				mergingSlotRemappings[i] = L1IRGlobalStateCreateSlot(self, localState, L1IRMakeSlot(SlotTypeForBlock(block->type), operands[0], operands[1], operands[2]));
				break;
		}
	}
	
	finish:

	resultLocalAddress = mergingSlotRemappings[block->native.slotCount - 1];

	free(mergingSlotRemappings);
	
	resultComputed:
	
	localState->callDepth--;

	L1IRLocalAddress retainedLocalAddresses[2] = {resultLocalAddress, finalArgumentLocalAddress};
	L1IRGlobalStatePopGCBarrier(self, localState, retainedLocalAddresses, finalArgumentLocalAddressOut ? 2 : 1);
	
	if (finalArgumentLocalAddressOut)
		*finalArgumentLocalAddressOut = finalArgumentLocalAddress;

	return resultLocalAddress;
}

uint16_t L1IRGlobalStateCall(L1IRGlobalState* self, L1IRLocalState* localState, L1IRLocalAddress calleeAddress, L1IRLocalAddress argumentLocalAddress)
{
	return L1IRGlobalStateEvaluate(self, localState,  L1IRGlobalStateEvaluationFlagHasArgument, calleeAddress, argumentLocalAddress, 0, NULL);
}
