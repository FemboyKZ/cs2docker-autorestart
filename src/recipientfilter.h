/**
 * Minimal self-contained recipient filter for broadcasting user messages.
 * Avoids depending on a full player-manager; recipients are added by slot.
 */

#pragma once

#include "irecipientfilter.h"

class CSimpleRecipientFilter : public IRecipientFilter
{
public:
	CSimpleRecipientFilter(NetChannelBufType_t nBufType = BUF_RELIABLE, bool bInitMessage = false)
		: m_nBufType(nBufType), m_bInitMessage(bInitMessage)
	{
	}

	~CSimpleRecipientFilter() override {}

	NetChannelBufType_t GetNetworkBufType(void) const override
	{
		return m_nBufType;
	}

	bool IsInitMessage(void) const override
	{
		return m_bInitMessage;
	}

	const CPlayerBitVec &GetRecipients(void) const override
	{
		return m_Recipients;
	}

	CPlayerSlot GetPredictedPlayerSlot(void) const override
	{
		return -1;
	}

	void AddRecipient(int slot)
	{
		if (slot >= 0 && slot < ABSOLUTE_PLAYER_LIMIT)
		{
			m_Recipients.Set(slot);
		}
	}

private:
	NetChannelBufType_t m_nBufType;
	bool m_bInitMessage;
	CPlayerBitVec m_Recipients;
};
