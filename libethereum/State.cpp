/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Foobar is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file State.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include <secp256k1.h>
#if WIN32
#pragma warning(push)
#pragma warning(disable:4244)
#else
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <sha.h>
#include <sha3.h>
#include <ripemd.h>
#if WIN32
#pragma warning(pop)
#else
#endif
#include <time.h>
#include <random>
#include "BlockChain.h"
#include "Instruction.h"
#include "Exceptions.h"
#include "Dagger.h"
#include "State.h"
using namespace std;
using namespace eth;

u256 const State::c_stepFee = 0;
u256 const State::c_dataFee = 0;
u256 const State::c_memoryFee = 0;
u256 const State::c_extroFee = 0;
u256 const State::c_cryptoFee = 0;
u256 const State::c_newContractFee = 0;
u256 const State::c_txFee = 0;
u256 const State::c_blockReward = 0;

State::State(Address _coinbaseAddress): m_state(&m_db), m_ourAddress(_coinbaseAddress)
{
	secp256k1_start();
	m_previousBlock = BlockInfo::genesis();
	m_currentBlock.coinbaseAddress = m_ourAddress;

	ldb::Options o;
	ldb::DB* db = nullptr;
	ldb::DB::Open(o, string(getenv("HOME")) + "/.ethereum++/state", &db);

	m_db.setDB(db);
	m_state.init();
	m_state.setRoot(m_currentBlock.stateRoot);
}

void State::ensureCached(Address _a, bool _requireMemory) const
{
	auto it = m_cache.find(_a);
	if (it == m_cache.end())
	{
		// populate basic info.
		string stateBack = m_state.at(_a);
		RLP state(stateBack);
		AddressState s;
		if (state.itemCount() == 2)
			s = AddressState(state[0].toInt<u256>(), state[1].toInt<u256>());
		else
			s = AddressState(state[0].toInt<u256>(), state[1].toInt<u256>(), state[2].toHash<h256>());
		bool ok;
		tie(it, ok) = m_cache.insert(make_pair(_a, s));
	}
	if (_requireMemory && !it->second.haveMemory())
	{
		// Populate memory.
		assert(it->second.type() == AddressType::Contract);
		TrieDB<u256, Overlay> memdb(const_cast<Overlay*>(&m_db), it->second.oldRoot());		// promise we won't alter the overlay! :)
		map<u256, u256>& mem = it->second.takeMemory();
		for (auto const& i: memdb)
			mem[i.first] = RLP(i.second).toInt<u256>();
	}
}

void State::commit()
{
	for (auto const& i: m_cache)
		if (i.second.type() == AddressType::Dead)
			m_state.remove(i.first);
		else
		{
			RLPStream s;
			s << i.second.balance() << i.second.nonce();
			if (i.second.type() == AddressType::Contract)
			{
				if (i.second.haveMemory())
				{
					TrieDB<u256, Overlay> memdb(&m_db);
					memdb.init();
					for (auto const& j: i.second.memory())
						if (j.second)
							memdb.insert(j.first, rlp(j.second));	// TODO: CHECK: check this isn't RLP or compact
					s << memdb.root();
				}
				else
					s << i.second.oldRoot();
			}
			m_state.insert(i.first, &s.out());
		}
	m_cache.clear();
}

void State::sync(BlockChain const& _bc)
{
	sync(_bc, _bc.currentHash());
}

void State::sync(BlockChain const& _bc, h256 _block)
{
	// BLOCK
	BlockInfo bi;
	try
	{
		auto b = _bc.block(_block);
		bi.populate(b);
		bi.verifyInternals(_bc.block(_block));
	}
	catch (...)
	{
		// TODO: Slightly nicer handling? :-)
		cerr << "ERROR: Corrupt block-chain! Delete your block-chain DB and restart." << endl;
		exit(1);
	}

	if (bi == m_currentBlock)
	{
		// We mined the last block.
		// Our state is good - we just need to move on to next.
		m_previousBlock = m_currentBlock;
		resetCurrent();
		m_currentNumber++;
	}
	else if (bi == m_previousBlock)
	{
		// No change since last sync.
		// Carry on as we were.
	}
	else
	{
		// New blocks available, or we've switched to a different branch. All change.
		// Find most recent state dump and replay what's left.
		// (Most recent state dump might end up being genesis.)
		std::vector<h256> l = _bc.blockChain(h256Set());

		if (l.back() == BlockInfo::genesis().hash)
		{
			// Reset to genesis block.
			m_previousBlock = BlockInfo::genesis();
		}
		else
		{
			// TODO: Begin at a restore point.
		}

		// Iterate through in reverse, playing back each of the blocks.
		for (auto it = next(l.cbegin()); it != l.cend(); ++it)
			playback(_bc.block(*it));

		m_currentNumber = _bc.details(_bc.currentHash()).number + 1;
		resetCurrent();
	}
}

void State::resetCurrent()
{
	m_transactions.clear();
	m_cache.clear();
	m_currentBlock = BlockInfo();
	m_currentBlock.coinbaseAddress = m_ourAddress;
	m_currentBlock.stateRoot = m_previousBlock.stateRoot;
}

void State::sync(TransactionQueue& _tq)
{
	// TRANSACTIONS
	auto ts = _tq.transactions();
	for (auto const& i: ts)
	{
		if (!m_transactions.count(i.first))
		{
			// don't have it yet! Execute it now.
			try
			{
				Transaction t(i.second);
				execute(t, t.sender());
			}
			catch (InvalidNonce in)
			{
				if (in.required > in.candidate)
					// too old
					_tq.drop(i.first);
			}
			catch (...)
			{
				// Something else went wrong - drop it.
				_tq.drop(i.first);
			}
		}
	}
}

u256 State::playback(bytesConstRef _block)
{
	try
	{
		m_currentBlock.populate(_block);
		m_currentBlock.verifyInternals(_block);
		return playback(_block, BlockInfo());
	}
	catch (...)
	{
		// TODO: Slightly nicer handling? :-)
		cerr << "ERROR: Corrupt block-chain! Delete your block-chain DB and restart." << endl;
		exit(1);
	}
}

u256 State::playback(bytesConstRef _block, BlockInfo const& _bi, BlockInfo const& _parent, BlockInfo const& _grandParent)
{
	m_currentBlock = _bi;
	m_previousBlock = _parent;
	return playback(_block, _grandParent);
}

u256 State::playback(bytesConstRef _block, BlockInfo const& _grandParent)
{
	if (m_currentBlock.parentHash != m_previousBlock.hash)
		throw InvalidParentHash();

	// All ok with the block generally. Play back the transactions now...
	for (auto const& i: RLP(_block)[1])
		execute(i.data());

	// Initialise total difficulty calculation.
	u256 tdIncrease = m_currentBlock.difficulty;

	// Check uncles & apply their rewards to state.
	Addresses rewarded;
	for (auto const& i: RLP(_block)[2])
	{
		BlockInfo uncle(i.data());
		if (m_previousBlock.parentHash != uncle.parentHash)
			throw InvalidUncle();
		if (_grandParent)
			uncle.verifyParent(_grandParent);
		tdIncrease += uncle.difficulty;
		rewarded.push_back(uncle.coinbaseAddress);
	}
	applyRewards(rewarded);

	// Commit all cached state changes to the state trie.
	commit();

	// Hash the state trie and check against the state_root hash in m_currentBlock.
	if (m_currentBlock.stateRoot != rootHash())
	{
		// Rollback the trie.
		m_db.rollback();
		throw InvalidStateRoot();
	}

	// Commit the new trie to disk.
	m_db.commit();

	m_previousBlock = m_currentBlock;
	resetCurrent();

	return tdIncrease;
}

// @returns the block that represents the difference between m_previousBlock and m_currentBlock.
// (i.e. all the transactions we executed).
void State::prepareToMine(BlockChain const& _bc)
{
	RLPStream uncles;
	if (m_previousBlock != BlockInfo::genesis())
	{
		// Find uncles if we're not a direct child of the genesis.
		auto us = _bc.details(m_previousBlock.parentHash).children;
		uncles.appendList(us.size());
		for (auto const& u: us)
			BlockInfo(_bc.block(u)).fillStream(uncles, true);
	}
	else
		uncles.appendList(0);

	RLPStream txs(m_transactions.size());
	for (auto const& i: m_transactions)
		i.second.fillStream(txs);

	txs.swapOut(m_currentTxs);
	uncles.swapOut(m_currentUncles);

	m_currentBlock.sha3Transactions = sha3(m_currentTxs);
	m_currentBlock.sha3Uncles = sha3(m_currentUncles);

	// Commit any and all changes to the trie that are in the cache, then update the state root accordingly.
	commit();
	m_currentBlock.stateRoot = m_state.root();
}

bool State::mine(uint _msTimeout)
{
	// Update timestamp according to clock.
	m_currentBlock.timestamp = time(0);

	// Update difficulty according to timestamp.
	m_currentBlock.difficulty = m_currentBlock.calculateDifficulty(m_previousBlock);

	// TODO: Miner class that keeps dagger between mine calls (or just non-polling mining).
	if (m_dagger.mine(/*out*/m_currentBlock.nonce, m_currentBlock.headerHashWithoutNonce(), m_currentBlock.difficulty, _msTimeout))
	{
		// Got it! Compile block:
		RLPStream ret;
		ret.appendList(3);
		m_currentBlock.fillStream(ret, true);
		ret.appendRaw(m_currentTxs);
		ret.appendRaw(m_currentUncles);
		ret.swapOut(m_currentBytes);
		return true;
	}

	return false;
}

bool State::isNormalAddress(Address _id) const
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return false;
	return it->second.type() == AddressType::Normal;
}

bool State::isContractAddress(Address _id) const
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return false;
	return it->second.type() == AddressType::Contract;
}

u256 State::balance(Address _id) const
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return 0;
	return it->second.balance();
}

void State::noteSending(Address _id)
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		m_cache[_id] = AddressState(0, 1);
	else
		it->second.incNonce();
}

void State::addBalance(Address _id, u256 _amount)
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		m_cache[_id] = AddressState(_amount, 0);
	else
		it->second.addBalance(_amount);
}

void State::subBalance(Address _id, bigint _amount)
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end() || (bigint)it->second.balance() < _amount)
		throw NotEnoughCash();
	else
		it->second.addBalance(-_amount);
}

u256 State::transactionsFrom(Address _id) const
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return 0;
	else
		return it->second.nonce();
}

u256 State::contractMemory(Address _id, u256 _memory) const
{
	ensureCached(_id);
	auto it = m_cache.find(_id);
	if (it == m_cache.end() || it->second.type() != AddressType::Contract)
		return 0;
	else if (it->second.haveMemory())
	{
		auto mit = it->second.memory().find(_memory);
		if (mit == it->second.memory().end())
			return 0;
		return mit->second;
	}
	// Memory not cached - just grab one item from the DB rather than cache the lot.
	TrieDB<u256, Overlay> memdb(const_cast<Overlay*>(&m_db), it->second.oldRoot());			// promise we won't change the overlay! :)
	return RLP(memdb.at(_memory)).toInt<u256>();	// TODO: CHECK: check if this is actually an RLP decode
}

bool State::execute(bytesConstRef _rlp)
{
	// Entry point for a user-executed transaction.
	try
	{
		Transaction t(_rlp);
		execute(t, t.sender());

		// Add to the user-originated transactions that we've executed.
		// NOTE: Here, contract-originated transactions will not get added to the transaction list.
		// If this is wrong, move this line into execute(Transaction const& _t, Address _sender) and
		// don't forget to allow unsigned transactions in the tx list if they concur with the script execution.
		m_transactions.insert(make_pair(t.sha3(), t));

		return true;
	}
	catch (...)
	{
		return false;
	}
}

void State::applyRewards(Addresses const& _uncleAddresses)
{
	u256 r = c_blockReward;
	for (auto const& i: _uncleAddresses)
	{
		addBalance(i, c_blockReward * 4 / 3);
		r += c_blockReward / 8;
	}
	addBalance(m_currentBlock.coinbaseAddress, r);
}

void State::execute(Transaction const& _t, Address _sender)
{
	// Entry point for a contract-originated transaction.

	// Ignore invalid transactions.
	auto nonceReq = transactionsFrom(_sender);
	if (_t.nonce != nonceReq)
		throw InvalidNonce(nonceReq, _t.nonce);

	// Not considered invalid - just pointless.
	if (balance(_sender) < _t.value + _t.fee)
		throw NotEnoughCash();

	// Increment associated nonce for sender.
	noteSending(_sender);

	if (_t.receiveAddress)
	{
		subBalance(_sender, _t.value + _t.fee);
		addBalance(_t.receiveAddress, _t.value);
		addBalance(m_currentBlock.coinbaseAddress, _t.fee);

		if (isContractAddress(_t.receiveAddress))
		{
			MinerFeeAdder feeAdder({this, 0});	// will add fee on destruction.
			execute(_t.receiveAddress, _sender, _t.value, _t.fee, _t.data, &feeAdder.fee);
		}
	}
	else
	{
		// Try to make a new contract
		if (_t.fee < _t.data.size() * c_memoryFee + c_newContractFee)
			throw FeeTooSmall();

		Address newAddress = low160(_t.sha3());

		if (isContractAddress(newAddress) || isNormalAddress(newAddress))
			throw ContractAddressCollision();

		// All OK - set it up.
		m_cache[newAddress] = AddressState(0, 0, sha3(RLPNull));
		auto& mem = m_cache[newAddress].takeMemory();
		for (uint i = 0; i < _t.data.size(); ++i)
			mem[i] = _t.data[i];
		subBalance(_sender, _t.value + _t.fee);
		addBalance(newAddress, _t.value);
		addBalance(m_currentBlock.coinbaseAddress, _t.fee);
	}
}

// Convert from a 256-bit integer stack/memory entry into a 160-bit Address hash.
// Currently we just pull out the left (high-order in BE) 160-bits.
// TODO: CHECK: check that this is correct.
inline Address asAddress(u256 _item)
{
	return left160(h256(_item));
}

void State::execute(Address _myAddress, Address _txSender, u256 _txValue, u256 _txFee, u256s const& _txData, u256* _totalFee)
{
	std::vector<u256> stack;

	// Set up some local functions.
	auto require = [&](u256 _n)
	{
		if (stack.size() < _n)
			throw StackTooSmall(_n, stack.size());
	};
	ensureCached(_myAddress, true);
	auto& myMemory = m_cache[_myAddress].takeMemory();

	auto mem = [&](u256 _n) -> u256
	{
		auto i = myMemory.find(_n);
		return i == myMemory.end() ? 0 : i->second;
	};
	auto setMem = [&](u256 _n, u256 _v)
	{
		if (_v)
			myMemory[_n] = _v;
		else
			myMemory.erase(_n);
	};

	u256 curPC = 0;
	u256 nextPC = 1;
	u256 stepCount = 0;
	for (bool stopped = false; !stopped; curPC = nextPC, nextPC = curPC + 1)
	{
		stepCount++;

		bigint minerFee = stepCount > 16 ? c_stepFee : 0;
		bigint voidFee = 0;

		auto rawInst = mem(curPC);
		if (rawInst > 0xff)
			throw BadInstruction();
		Instruction inst = (Instruction)(uint8_t)rawInst;

		switch (inst)
		{
		case Instruction::STORE:
			require(2);
			if (!mem(stack.back()) && stack[stack.size() - 2])
				voidFee += c_memoryFee;
			if (mem(stack.back()) && !stack[stack.size() - 2])
				voidFee -= c_memoryFee;
			// continue on to...
		case Instruction::LOAD:
			minerFee += c_dataFee;
			break;

		case Instruction::EXTRO:
		case Instruction::BALANCE:
			minerFee += c_extroFee;
			break;

		case Instruction::MKTX:
			minerFee += c_txFee;
			break;

		case Instruction::SHA256:
		case Instruction::RIPEMD160:
		case Instruction::ECMUL:
		case Instruction::ECADD:
		case Instruction::ECSIGN:
		case Instruction::ECRECOVER:
		case Instruction::ECVALID:
			minerFee += c_cryptoFee;
			break;
		default:
			break;
		}

		if (minerFee + voidFee > balance(_myAddress))
			throw NotEnoughCash();
		subBalance(_myAddress, minerFee + voidFee);
		*_totalFee += (u256)minerFee;

		switch (inst)
		{
		case Instruction::ADD:
			//pops two items and pushes S[-1] + S[-2] mod 2^256.
			require(2);
			stack[stack.size() - 2] += stack.back();
			stack.pop_back();
			break;
		case Instruction::MUL:
			//pops two items and pushes S[-1] * S[-2] mod 2^256.
			require(2);
			stack[stack.size() - 2] *= stack.back();
			stack.pop_back();
			break;
		case Instruction::SUB:
			require(2);
			stack[stack.size() - 2] = stack.back() - stack[stack.size() - 2];
			stack.pop_back();
			break;
		case Instruction::DIV:
			require(2);
			stack[stack.size() - 2] = stack.back() / stack[stack.size() - 2];
			stack.pop_back();
			break;
		case Instruction::SDIV:
			require(2);
			(s256&)stack[stack.size() - 2] = (s256&)stack.back() / (s256&)stack[stack.size() - 2];
			stack.pop_back();
			break;
		case Instruction::MOD:
			require(2);
			stack[stack.size() - 2] = stack.back() % stack[stack.size() - 2];
			stack.pop_back();
			break;
		case Instruction::SMOD:
			require(2);
			(s256&)stack[stack.size() - 2] = (s256&)stack.back() % (s256&)stack[stack.size() - 2];
			stack.pop_back();
			break;
		case Instruction::EXP:
		{
			// TODO: better implementation?
			require(2);
			auto n = stack.back();
			auto x = stack[stack.size() - 2];
			stack.pop_back();
			for (u256 i = 0; i < x; ++i)
				n *= n;
			stack.back() = n;
			break;
		}
		case Instruction::NEG:
			require(1);
			stack.back() = ~(stack.back() - 1);
			break;
		case Instruction::LT:
			require(2);
			stack[stack.size() - 2] = stack.back() < stack[stack.size() - 2] ? 1 : 0;
			stack.pop_back();
			break;
		case Instruction::LE:
			require(2);
			stack[stack.size() - 2] = stack.back() <= stack[stack.size() - 2] ? 1 : 0;
			stack.pop_back();
			break;
		case Instruction::GT:
			require(2);
			stack[stack.size() - 2] = stack.back() > stack[stack.size() - 2] ? 1 : 0;
			stack.pop_back();
			break;
		case Instruction::GE:
			require(2);
			stack[stack.size() - 2] = stack.back() >= stack[stack.size() - 2] ? 1 : 0;
			stack.pop_back();
			break;
		case Instruction::EQ:
			require(2);
			stack[stack.size() - 2] = stack.back() == stack[stack.size() - 2] ? 1 : 0;
			stack.pop_back();
			break;
		case Instruction::NOT:
			require(1);
			stack.back() = stack.back() ? 0 : 1;
			stack.pop_back();
			break;
		case Instruction::MYADDRESS:
			stack.push_back((u160)_myAddress);
			break;
		case Instruction::TXSENDER:
			stack.push_back((u160)_txSender);
			break;
		case Instruction::TXVALUE:
			stack.push_back(_txValue);
			break;
		case Instruction::TXFEE:
			stack.push_back(_txFee);
			break;
		case Instruction::TXDATAN:
			stack.push_back(_txData.size());
			break;
		case Instruction::TXDATA:
			require(1);
			stack.back() = stack.back() < _txData.size() ? _txData[(uint)stack.back()] : 0;
			break;
		case Instruction::BLK_PREVHASH:
			stack.push_back(m_previousBlock.hash);
			break;
		case Instruction::BLK_COINBASE:
			stack.push_back((u160)m_currentBlock.coinbaseAddress);
			break;
		case Instruction::BLK_TIMESTAMP:
			stack.push_back(m_currentBlock.timestamp);
			break;
		case Instruction::BLK_NUMBER:
			stack.push_back(m_currentNumber);
			break;
		case Instruction::BLK_DIFFICULTY:
			stack.push_back(m_currentBlock.difficulty);
			break;
		case Instruction::SHA256:
		{
			uint s = (uint)min(stack.back(), (u256)(stack.size() - 1) * 32);
			stack.pop_back();

			CryptoPP::SHA256 digest;
			uint i = 0;
			for (; s; s = (s >= 32 ? s - 32 : 0), i += 32)
			{
				bytes b = toBigEndian(stack.back());
				digest.Update(b.data(), (int)min<u256>(32, s));			// b.size() == 32
				stack.pop_back();
			}
			array<byte, 32> final;
			digest.TruncatedFinal(final.data(), 32);
			stack.push_back(fromBigEndian<u256>(final));
			break;
		}
		case Instruction::RIPEMD160:
		{
			uint s = (uint)min(stack.back(), (u256)(stack.size() - 1) * 32);
			stack.pop_back();

			CryptoPP::RIPEMD160 digest;
			uint i = 0;
			for (; s; s = (s >= 32 ? s - 32 : 0), i += 32)
			{
				bytes b = toBigEndian(stack.back());
				digest.Update(b.data(), (int)min<u256>(32, s));			// b.size() == 32
				stack.pop_back();
			}
			array<byte, 20> final;
			digest.TruncatedFinal(final.data(), 20);
			// NOTE: this aligns to right of 256-bit container (low-order bytes).
			// This won't work if they're treated as byte-arrays and thus left-aligned in a 256-bit container.
			stack.push_back((u256)fromBigEndian<u160>(final));
			break;
		}
		case Instruction::ECMUL:
		{
			// ECMUL - pops three items.
			// If (S[-2],S[-1]) are a valid point in secp256k1, including both coordinates being less than P, pushes (S[-1],S[-2]) * S[-3], using (0,0) as the point at infinity.
			// Otherwise, pushes (0,0).
			require(3);

			bytes pub(1, 4);
			pub += toBigEndian(stack[stack.size() - 2]);
			pub += toBigEndian(stack.back());
			stack.pop_back();
			stack.pop_back();

			bytes x = toBigEndian(stack.back());
			stack.pop_back();

			if (secp256k1_ecdsa_pubkey_verify(pub.data(), (int)pub.size()))	// TODO: Check both are less than P.
			{
				secp256k1_ecdsa_pubkey_tweak_mul(pub.data(), (int)pub.size(), x.data());
				stack.push_back(fromBigEndian<u256>(bytesConstRef(&pub).cropped(1, 32)));
				stack.push_back(fromBigEndian<u256>(bytesConstRef(&pub).cropped(33, 32)));
			}
			else
			{
				stack.push_back(0);
				stack.push_back(0);
			}
			break;
		}
		case Instruction::ECADD:
		{
			// ECADD - pops four items and pushes (S[-4],S[-3]) + (S[-2],S[-1]) if both points are valid, otherwise (0,0).
			require(4);

			bytes pub(1, 4);
			pub += toBigEndian(stack[stack.size() - 2]);
			pub += toBigEndian(stack.back());
			stack.pop_back();
			stack.pop_back();

			bytes tweak(1, 4);
			tweak += toBigEndian(stack[stack.size() - 2]);
			tweak += toBigEndian(stack.back());
			stack.pop_back();
			stack.pop_back();

			if (secp256k1_ecdsa_pubkey_verify(pub.data(),(int) pub.size()) && secp256k1_ecdsa_pubkey_verify(tweak.data(),(int) tweak.size()))
			{
				secp256k1_ecdsa_pubkey_tweak_add(pub.data(), (int)pub.size(), tweak.data());
				stack.push_back(fromBigEndian<u256>(bytesConstRef(&pub).cropped(1, 32)));
				stack.push_back(fromBigEndian<u256>(bytesConstRef(&pub).cropped(33, 32)));
			}
			else
			{
				stack.push_back(0);
				stack.push_back(0);
			}
			break;
		}
		case Instruction::ECSIGN:
		{
			require(2);
			bytes sig(64);
			int v = 0;

			u256 msg = stack.back();
			stack.pop_back();
			u256 priv = stack.back();
			stack.pop_back();
			bytes nonce = toBigEndian(Transaction::kFromMessage(msg, priv));

			if (!secp256k1_ecdsa_sign_compact(toBigEndian(msg).data(), 64, sig.data(), toBigEndian(priv).data(), nonce.data(), &v))
				throw InvalidSignature();

			stack.push_back(v + 27);
			stack.push_back(fromBigEndian<u256>(bytesConstRef(&sig).cropped(0, 32)));
			stack.push_back(fromBigEndian<u256>(bytesConstRef(&sig).cropped(32)));
			break;
		}
		case Instruction::ECRECOVER:
		{
			require(4);

			bytes sig = toBigEndian(stack[stack.size() - 2]) + toBigEndian(stack.back());
			stack.pop_back();
			stack.pop_back();
			int v = (int)stack.back();
			stack.pop_back();
			bytes msg = toBigEndian(stack.back());
			stack.pop_back();

			byte pubkey[65];
			int pubkeylen = 65;
			if (secp256k1_ecdsa_recover_compact(msg.data(), (int)msg.size(), sig.data(), pubkey, &pubkeylen, 0, v - 27))
			{
				stack.push_back(0);
				stack.push_back(0);
			}
			else
			{
				stack.push_back(fromBigEndian<u256>(bytesConstRef(&pubkey[1], 32)));
				stack.push_back(fromBigEndian<u256>(bytesConstRef(&pubkey[33], 32)));
			}
			break;
		}
		case Instruction::ECVALID:
		{
			require(2);
			bytes pub(1, 4);
			pub += toBigEndian(stack[stack.size() - 2]);
			pub += toBigEndian(stack.back());
			stack.pop_back();
			stack.pop_back();

			stack.back() = secp256k1_ecdsa_pubkey_verify(pub.data(), (int)pub.size()) ? 1 : 0;
			break;
		}
		case Instruction::SHA3:
		{
			uint s = (uint)min(stack.back(), (u256)(stack.size() - 1) * 32);
			stack.pop_back();

			CryptoPP::SHA3_256 digest;
			uint i = 0;
			for (; s; s = (s >= 32 ? s - 32 : 0), i += 32)
			{
				bytes b = toBigEndian(stack.back());
				digest.Update(b.data(), (int)min<u256>(32, s));			// b.size() == 32
				stack.pop_back();
			}
			array<byte, 32> final;
			digest.TruncatedFinal(final.data(), 32);
			stack.push_back(fromBigEndian<u256>(final));
			break;
		}
		case Instruction::PUSH:
		{
			stack.push_back(mem(curPC + 1));
			nextPC = curPC + 2;
			break;
		}
		case Instruction::POP:
			require(1);
			stack.pop_back();
			break;
		case Instruction::DUP:
			require(1);
			stack.push_back(stack.back());
			break;
		case Instruction::DUPN:
		{
			auto s = mem(curPC + 1);
			if (s == 0 || s > stack.size())
				throw OperandOutOfRange(1, stack.size(), s);
			stack.push_back(stack[stack.size() - (uint)s]);
			nextPC = curPC + 2;
			break;
		}
		case Instruction::SWAP:
		{
			require(2);
			auto d = stack.back();
			stack.back() = stack[stack.size() - 2];
			stack[stack.size() - 2] = d;
			break;
		}
		case Instruction::SWAPN:
		{
			require(1);
			auto d = stack.back();
			auto s = mem(curPC + 1);
			if (s == 0 || s > stack.size())
				throw OperandOutOfRange(1, stack.size(), s);
			stack.back() = stack[stack.size() - (uint)s];
			stack[stack.size() - (uint)s] = d;
			nextPC = curPC + 2;
			break;
		}
		case Instruction::LOAD:
			require(1);
			stack.back() = mem(stack.back());
			break;
		case Instruction::STORE:
			require(2);
			setMem(stack.back(), stack[stack.size() - 2]);
			stack.pop_back();
			stack.pop_back();
			break;
		case Instruction::JMP:
			require(1);
			nextPC = stack.back();
			stack.pop_back();
			break;
		case Instruction::JMPI:
			require(2);
			if (stack.back())
				nextPC = stack[stack.size() - 2];
			stack.pop_back();
			stack.pop_back();
			break;
		case Instruction::IND:
			stack.push_back(curPC);
			break;
		case Instruction::EXTRO:
		{
			require(2);
			auto memoryAddress = stack.back();
			stack.pop_back();
			Address contractAddress = asAddress(stack.back());
			stack.back() = contractMemory(contractAddress, memoryAddress);
			break;
		}
		case Instruction::BALANCE:
		{
			require(1);
			stack.back() = balance(asAddress(stack.back()));
			break;
		}
		case Instruction::MKTX:
		{
			require(4);

			Transaction t;
			t.receiveAddress = asAddress(stack.back());
			stack.pop_back();
			t.value = stack.back();
			stack.pop_back();
			t.fee = stack.back();
			stack.pop_back();

			auto itemCount = stack.back();
			stack.pop_back();
			if (stack.size() < itemCount)
				throw OperandOutOfRange(0, stack.size(), itemCount);
			t.data.reserve((uint)itemCount);
			for (auto i = 0; i < itemCount; ++i)
			{
				t.data.push_back(stack.back());
				stack.pop_back();
			}

			t.nonce = transactionsFrom(_myAddress);
			execute(t, _myAddress);

			break;
		}
		case Instruction::SUICIDE:
		{
			require(1);
			Address dest = asAddress(stack.back());
			u256 minusVoidFee = myMemory.size() * c_memoryFee;
			addBalance(dest, balance(_myAddress) + minusVoidFee);
			m_cache[_myAddress].kill();
			// ...follow through to...
		}
		case Instruction::STOP:
			return;
		default:
			throw BadInstruction();
		}
	}
}
