// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal BCM6838 PMC client (DQM mailbox mode), modelled on the Broadcom SDK
 * shared/opensource/pmc/impl2/pmc_drv.c (the 6838 PMC driver) but limited to
 * what the RDP driver needs.
 */
#include <linux/kernel.h>
#include <linux/spinlock.h>

#include "bcm_map_part.h"
#include "command.h"
#include "pmc_drv.h"

static DEFINE_SPINLOCK(pmc_lock);

static int pmc_send_and_wait(TCommand *cmd, TCommand *rsp)
{
	static u32 req_id = 1;
	int status = kPMC_COMMAND_TIMEOUT;
	unsigned long flags;

	spin_lock_irqsave(&pmc_lock, flags);

	cmd->word0.Bits.msgID = req_id;
	PMC->dqmQData[PMC_DQM_REQ_NUM].word[0] = cmd->word0.Reg32;
	PMC->dqmQData[PMC_DQM_REQ_NUM].word[1] = cmd->word1.Reg32;
	PMC->dqmQData[PMC_DQM_REQ_NUM].word[2] = cmd->u.cmdGenericParams.params[0];
	PMC->dqmQData[PMC_DQM_REQ_NUM].word[3] = cmd->u.cmdGenericParams.params[1];

	/* one-shot 5 ms PMC timer bounds the wait for the reply */
	PMC->ctrl.gpTmr2Ctl = (1 << 31) | (1 << 30) | (1 << 29) | 5000;
	while (!(PMC->dqm.notEmptySts & PMC_DQM_RPL_STS) &&
	       (PMC->ctrl.gpTmr2Ctl & (1 << 31)))
		;

	if (PMC->dqm.notEmptySts & PMC_DQM_RPL_STS) {
		rsp->word0.Reg32 = PMC->dqmQData[PMC_DQM_RPL_NUM].word[0];
		rsp->word1.Reg32 = PMC->dqmQData[PMC_DQM_RPL_NUM].word[1];
		rsp->u.cmdGenericParams.params[0] = PMC->dqmQData[PMC_DQM_RPL_NUM].word[2];
		rsp->u.cmdGenericParams.params[1] = PMC->dqmQData[PMC_DQM_RPL_NUM].word[3];
		if (rsp->word0.Bits.msgID == req_id)
			status = rsp->word0.Bits.error;
		else
			status = kPMC_MESSAGE_ID_MISMATCH;
	}
	req_id = (req_id + 1) & 0xff;

	spin_unlock_irqrestore(&pmc_lock, flags);
	return status;
}

static int pmc_send(int cmd_id, int dev_addr, int zone, u32 p0, u32 p1,
		    TCommand *rsp)
{
	TCommand cmd, dummy;

	cmd.word0.Reg32 = 0;
	cmd.word0.Bits.cmdID = cmd_id;
	cmd.word1.Reg32 = 0;
	cmd.word1.Bits.devAddr = dev_addr;
	cmd.word1.Bits.zoneIdx = zone;
	cmd.u.cmdGenericParams.params[0] = p0;
	cmd.u.cmdGenericParams.params[1] = p1;
	return pmc_send_and_wait(&cmd, rsp ? rsp : &dummy);
}

/*
 * The 6838 PMC firmware uses the SDK pmc/impl2 command table: it has no
 * cmdRevision and adds cmdTuneRunner after cmdGetSelect3.
 */
#define PMC6838_CMD_TUNE_RUNNER	69

int Ping(void)
{
	return pmc_send(cmdPing, 0, 0, 0, 0, NULL);
}

int TuneRunner(void)
{
	return pmc_send(PMC6838_CMD_TUNE_RUNNER, 0, 0, 0, 0, NULL);
}

int ReadBPCMRegister(int devAddr, int wordOffset, uint32 *value)
{
	TCommand rsp;
	int status = pmc_send(cmdReadBpcmReg, devAddr, 0, wordOffset, 0, &rsp);

	if (status == kPMC_NO_ERROR)
		*value = rsp.u.cmdResponse.word2;
	return status;
}

int ReadZoneRegister(int devAddr, int zone, int wordOffset, uint32 *value)
{
	TCommand rsp;
	int status;

	if ((unsigned int)wordOffset >= 4)
		return kPMC_INVALID_PARAM;
	status = pmc_send(cmdReadZoneReg, devAddr, zone, wordOffset, 0, &rsp);
	if (status == kPMC_NO_ERROR)
		*value = rsp.u.cmdResponse.word2;
	return status;
}

int WriteBPCMRegister(int devAddr, int wordOffset, uint32 value)
{
	return pmc_send(cmdWriteBpcmReg, devAddr, 0, wordOffset, value, NULL);
}

int PowerOnDevice(int devAddr)
{
	TCommand cmd, rsp;

	cmd.word0.Reg32 = 0;
	cmd.word0.Bits.cmdID = cmdPowerDevOnOff;
	cmd.word1.Reg32 = 0;
	cmd.word1.Bits.devAddr = devAddr;
	cmd.u.cmdGenericParams.params[0] = 0;
	cmd.u.cmdGenericParams.params[1] = 0;
	cmd.u.cmdPowerDevice.state = 1;
	return pmc_send_and_wait(&cmd, &rsp);
}
