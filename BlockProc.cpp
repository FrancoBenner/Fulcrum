#include "BlockProc.h"
#include "BTC.h"
#include "Util.h"

#include "bitcoin/transaction.h"
#include "robin_hood/robin_hood.h"

#include <QTextStream>

#include <algorithm>
#include <set>
#include <unordered_set>

/* static */ const TxHash PreProcessedBlock::nullhash;

/// fill this struct's data with all the txdata, etc from a bitcoin CBlock. Alternative to using the second c'tor.
void PreProcessedBlock::fill(BlockHeight blockHeight, size_t blockSize, const bitcoin::CBlock &b) {
    if (!header.IsNull() || !txInfos.empty())
        clear();
    height = blockHeight;
    sizeBytes = blockSize;
    header = b.GetBlockHeader();
    estimatedThisSizeBytes = sizeof(*this) + size_t(BTC::GetBlockHeaderSize());
    txInfos.reserve(b.vtx.size());
    robin_hood::unordered_flat_map<TxHash, unsigned, HashHasher> txHashToIndex;
    // run through all tx's, build inputs and outputs lists
    size_t txIdx = 0;
    for (const auto & tx : b.vtx) {
        // copy tx hash data for the tx
        TxInfo info;
        info.hash = BTC::Hash2ByteArrayRev(tx->GetHash());
        info.nInputs = IONum(tx->vin.size());
        info.nOutputs = IONum(tx->vout.size());
        // remember the tx hash -> index association for use later in this function
        txHashToIndex[info.hash] = unsigned(txIdx); // cheap copy + cheap hash func. should make this fast.

        // process outputs for this tx
        if (!tx->vout.empty())
            // remember output0 index for this txindex
            info.output0Index.emplace( unsigned(outputs.size()) );

        IONum outN = 0;
        for (const auto & out : tx->vout) {
            // save the outputs seen
            outputs.emplace_back(
                OutPt{ unsigned(txIdx), outN, out.nValue, {} }
            );
            estimatedThisSizeBytes += sizeof(OutPt);
            const size_t outputIdx = outputs.size()-1;
            if (const auto cscript = out.scriptPubKey;
                    !BTC::IsOpReturn(cscript))  ///< skip OP_RETURN
            {
                const HashX hashX = BTC::HashXFromCScript(cscript);
                // add this output to the hashX -> outputs association for later
                auto & ag = hashXAggregated[ hashX ];
                ag.outs.emplace_back( outputIdx );
                if (auto & vec = ag.txNumsInvolvingHashX; vec.empty() || vec.back() != txIdx)
                    vec.emplace_back(txIdx);
            }
            else {
                ++nOpReturns;
            }/*//use this clause if you want to actually save/process opreturn scripts:
              else {
                // OpReturn tracking...
                opreturns.emplace_back(OpReturn{unsigned(outputIdx), cscript});
            }*/
            ++outN;
        }

        // process inputs
        if (!tx->vin.empty())
            // remember input0Index position for this tx
            info.input0Index.emplace( unsigned(inputs.size()) );

        for (const auto & in : tx->vin) {
            // note we do place the coinbase tx here even though we ignore it later on -- we keep it to have accurate indices
            inputs.emplace_back(InputPt{
                    unsigned(txIdx),
                    BTC::Hash2ByteArrayRev(in.prevout.GetTxId()),  // .prevoutHash
                    uint16_t(in.prevout.GetN()), // .prevoutN
                    {}, // .parentTxOutIdx (start out undefined)
            });
            estimatedThisSizeBytes += sizeof(InputPt);
        }
        estimatedThisSizeBytes += sizeof(info) + size_t(info.hash.size());
        txInfos.emplace_back(std::move(info));
        ++txIdx;
    }

    // shrink inputs/outputs to fit now to conserve memory
    inputs.shrink_to_fit();
    outputs.shrink_to_fit();

    // at this point we have a partially constructed object. we must run through all the inputs again
    // and figure out which if any refer to tx's in this block, and assign those to our hashXIns.
    // Also: to save memory on txhash's for such inputs, we make sure the txhash refers to the same underlying
    // QByteArray data.
    size_t inIdx = 0;
    for (auto & inp : inputs) {
        if (const auto it = txHashToIndex.find(inp.prevoutHash); it != txHashToIndex.end()) {
            // this input refers to a tx in this block!
            const auto prevTxIdx = it->second;
            assert(prevTxIdx < txInfos.size() && prevTxIdx < b.vtx.size());
            const TxInfo & prevInfo = txInfos[prevTxIdx];
            inp.prevoutHash = prevInfo.hash; //<--- ensure shallow copy that points to same underlying data (saves memory)
            if (prevInfo.output0Index.has_value())
                inp.parentTxOutIdx.emplace( prevInfo.output0Index.value() + inp.prevoutN ); // save the index into the `outputs` array where the parent tx to this spend occurred
            else { assert(0); }
            auto & outp = outputs[ inp.parentTxOutIdx.value() ];
            outp.spentInInputIndex.emplace( inIdx ); // mark the output as spent by this index
            const auto & prevTx = b.vtx[prevTxIdx];
            assert(inp.prevoutN < prevTx->vout.size());
            if (const auto cscript = prevTx->vout[inp.prevoutN].scriptPubKey;  // grab prevOut address
                    !BTC::IsOpReturn(cscript))
            {
                // mark this input as involving this hashX
                const HashX hashX = BTC::HashXFromCScript(cscript);
                auto & ag = hashXAggregated[ hashX ];
                ag.ins.emplace_back(inIdx);
                if (auto & vec = ag.txNumsInvolvingHashX; vec.empty() || vec.back() != inp.txIdx)
                    vec.emplace_back(inp.txIdx);  // now that we resolved the input's spending address, mark this input's txIdx as having touched this hashX
            }
        }
        ++inIdx;
    }

    for (auto & [hashX, ag] : hashXAggregated ) {
        std::sort(ag.ins.begin(), ag.ins.end());
        std::sort(ag.outs.begin(), ag.outs.end());
        std::sort(ag.txNumsInvolvingHashX.begin(), ag.txNumsInvolvingHashX.end());
        auto last = std::unique(ag.txNumsInvolvingHashX.begin(), ag.txNumsInvolvingHashX.end());
        ag.txNumsInvolvingHashX.erase(last, ag.txNumsInvolvingHashX.end());
        ag.ins.shrink_to_fit();
        ag.outs.shrink_to_fit();
        ag.txNumsInvolvingHashX.shrink_to_fit();
        // tally up space usage
        estimatedThisSizeBytes +=
                sizeof(ag) + size_t(hashX.size()) + ag.ins.size() * sizeof(decltype(ag.ins)::value_type)
                + ag.outs.size() * sizeof(decltype(ag.outs)::value_type)
                + ag.txNumsInvolvingHashX.size() * sizeof(decltype(ag.txNumsInvolvingHashX)::value_type);
    }
}

QString PreProcessedBlock::toDebugString() const
{
    QString ret;
    {
        QTextStream ts(&ret, QIODevice::ReadOnly|QIODevice::Truncate|QIODevice::Text);
        ts << "<PreProcessedBlock --"
           << " height: " << height << " " << " size: " << sizeBytes << " header_nTime: " << header.nTime << " hash: " << header.GetHash().ToString().c_str()
           << " nTx: " << txInfos.size() << " nIns: " << inputs.size() << " nOuts: " << outputs.size() << " nScriptHash: " << hashXAggregated.size();
        int i = 0;
        for (const auto & [hashX, ag] : hashXAggregated) {
            ts << " (#" << i << " - " << hashX.toHex() << " - nIns: " << ag.ins.size() << " nOuts: " << ag.outs.size();
            for (size_t j = 0; j < ag.ins.size(); ++j) {
                const auto idx = ag.ins[j];
                const auto & theInput [[maybe_unused]] = inputs[idx];
                assert(theInput.parentTxOutIdx.has_value() && txHashForOutputIdx(theInput.parentTxOutIdx.value()) == theInput.prevoutHash);
                ts << " {in# " << j << " - " << inputs[idx].prevoutHash.toHex() << ":" << inputs[idx].prevoutN
                   << ", spent in " << txHashForInputIdx(idx).toHex() << ":" << numForInputIdx(idx).value_or(999999) << " }";
            }
            for (size_t j = 0; j < ag.outs.size(); ++j) {
                const auto idx = ag.outs[j];
                ts << " {out# " << j << " - " << txHashForOutputIdx(idx).toHex() << ":" << outputs[idx].outN << " amt: " << outputs[idx].amount.ToString().c_str() << " }";
            }
            ts << ")";
            ++i;
        }
        /*
        ts << " opreturns: " << opreturns.size();
        i = 0;
        for (const auto & op : opreturns) {
            ts << " (#" << i << " - " << txInfos[outputs[op.outIdx].txIdx].hash.toHex() << ")";
            ++i;
        }*/
        ts << " >";
    }
    return ret;
}

/// convenience factory static method: given a block, return a shard_ptr instance of this struct
/*static*/
PreProcessedBlockPtr PreProcessedBlock::makeShared(unsigned height_, size_t size, const bitcoin::CBlock &block)
{
    return std::make_shared<PreProcessedBlock>(height_, size, block);
}


// very much a work in progress. this needs to also consult the UTXO set to be complete. For now we just
// have this here for reference.
std::vector<std::unordered_set<HashX, HashHasher>>
PreProcessedBlock::hashXsByTx() const
{
    std::vector<std::unordered_set<HashX, HashHasher>> ret(txInfos.size());
    for (const auto & [hashX, ag] : hashXAggregated) {
        // scan all outputs and add this hashX
        for (const auto outIdx : ag.outs) {
            ret[outputs[outIdx].txIdx].insert(hashX); // cheap shallow copy
        }
        // scan all inputs and add this hashX
        for (const auto inIdx : ag.ins) {
            ret[inputs[inIdx].txIdx].insert(hashX);
        }
    }
    return ret;
}
