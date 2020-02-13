// Copyright 2020 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wallet/core/common.h"
#include "wallet/core/wallet_network.h"
#include "wallet/core/wallet.h"
#include "wallet/transactions/lelantus/push_transaction.h"
#include "wallet/transactions/lelantus/pull_transaction.h"
#include "keykeeper/local_private_key_keeper.h"
#include "wallet/core/simple_transaction.h"
#include "core/unittest/mini_blockchain.h"
#include "utility/test_helpers.h"

#include "node/node.h"

#include "test_helpers.h"

#include <boost/filesystem.hpp>

using namespace beam;
using namespace std;
using namespace ECC;

WALLET_TEST_INIT

#include "wallet_test_environment.cpp"

namespace
{
    const AmountList kDefaultTestAmounts = { 5000, 2000, 1000, 9000 };

    class ScopedGlobalRules
    {
    public:
        ScopedGlobalRules()
        {
            m_rules = Rules::get();
        }

        ~ScopedGlobalRules()
        {
            Rules::get() = m_rules;
        }
    private:
        Rules m_rules;
    };

    void InitOwnNodeToTest(Node& node, const ByteBuffer& binaryTreasury, Node::IObserver* observer, Key::IPKdf::Ptr ownerKey, uint16_t port = 32125, uint32_t powSolveTime = 1000, const std::string& path = "mytest.db", const std::vector<io::Address>& peers = {}, bool miningNode = true)
    {
        node.m_Keys.m_pOwner = ownerKey;
        node.m_Cfg.m_Treasury = binaryTreasury;
        ECC::Hash::Processor() << Blob(node.m_Cfg.m_Treasury) >> Rules::get().TreasuryChecksum;

        boost::filesystem::remove(path);
        node.m_Cfg.m_sPathLocal = path;
        node.m_Cfg.m_Listen.port(port);
        node.m_Cfg.m_Listen.ip(INADDR_ANY);
        node.m_Cfg.m_MiningThreads = miningNode ? 1 : 0;
        node.m_Cfg.m_VerificationThreads = 1;
        node.m_Cfg.m_TestMode.m_FakePowSolveTime_ms = powSolveTime;
        node.m_Cfg.m_Connect = peers;

        node.m_Cfg.m_Dandelion.m_AggregationTime_ms = 0;
        node.m_Cfg.m_Dandelion.m_OutputsMin = 0;
        //Rules::get().Maturity.Coinbase = 1;
        Rules::get().FakePoW = true;

        node.m_Cfg.m_Observer = observer;
        Rules::get().UpdateChecksum();
        node.Initialize();
        node.m_PostStartSynced = true;
    }
}

void TestSimpleTx()
{
    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);

    int completedCount = 4;
    auto completeAction = [&mainReactor, &completedCount](auto)
    {
        --completedCount;
        if (completedCount == 0)
        {
            mainReactor->stop();
        }
    };

    auto senderWalletDB = createSenderWalletDB(0, 0);
    auto binaryTreasury = createTreasury(senderWalletDB, kDefaultTestAmounts);
    TestWalletRig sender("sender", senderWalletDB, completeAction, TestWalletRig::RegularWithoutPoWBbs);

    auto pushTxCreator = std::make_shared<lelantus::PushTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PushTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pushTxCreator));

    auto pullTxCreator = std::make_shared<lelantus::PullTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PullTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pullTxCreator));

    Node node;
    NodeObserver observer([&]()
    {
        auto cursor = node.get_Processor().m_Cursor;
        if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 3)
        {
            auto parameters = lelantus::CreatePushTransactionParameters(sender.m_WalletID)
                .SetParameter(TxParameterID::Amount, 3800)
                .SetParameter(TxParameterID::Fee, 1200);

            sender.m_Wallet.StartTransaction(parameters);
        }
        else if (cursor.m_Sid.m_Height == 30)
        {
            auto parameters = lelantus::CreatePushTransactionParameters(sender.m_WalletID)
                .SetParameter(TxParameterID::Amount, 7800)
                .SetParameter(TxParameterID::Fee, 1200);

            sender.m_Wallet.StartTransaction(parameters);
        }
        else if (cursor.m_Sid.m_Height == 40)
        {
            auto parameters = lelantus::CreatePullTransactionParameters(sender.m_WalletID)
                .SetParameter(TxParameterID::Amount, 6600)
                .SetParameter(TxParameterID::AmountList, AmountList{ 4600, 2000 })
                .SetParameter(TxParameterID::Fee, 1200)
                .SetParameter(TxParameterID::WindowBegin, 0U)
                .SetParameter(TxParameterID::ShieldedOutputId, 1U);

            sender.m_Wallet.StartTransaction(parameters);
        }
        else if (cursor.m_Sid.m_Height == 50)
        {
            auto parameters = lelantus::CreatePullTransactionParameters(sender.m_WalletID)
                .SetParameter(TxParameterID::Amount, 2600)
                .SetParameter(TxParameterID::Fee, 1200)
                .SetParameter(TxParameterID::WindowBegin, 0U)
                .SetParameter(TxParameterID::ShieldedOutputId, 0U);

            sender.m_Wallet.StartTransaction(parameters);
        }
        else if (cursor.m_Sid.m_Height == 70)
        {
            mainReactor->stop();
        }
    });

    InitOwnNodeToTest(node, binaryTreasury, &observer, sender.m_WalletDB->get_MasterKdf(), 32125, 200);

    mainReactor->run();

    WALLET_CHECK(completedCount == 0);
    auto txHistory = sender.m_WalletDB->getTxHistory(TxType::ALL);
    WALLET_CHECK(std::all_of(txHistory.begin(), txHistory.end(), [](const auto& tx)
        {
            return (tx.m_txType == TxType::PushTransaction || tx.m_txType == TxType::PullTransaction) && tx.m_status == TxStatus::Completed;
        }));
}

void TestManyTransactons()
{
    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);

    int completedCount = 2000;
    auto completeAction = [&mainReactor, &completedCount](auto)
    {
        --completedCount;
        if (completedCount == 0)
        {
            mainReactor->stop();
        }
    };

    constexpr size_t kAmount = 2000;
    constexpr Amount kNominalCoin = 5000;
    AmountList testAmount(kAmount, kNominalCoin);

    auto senderWalletDB = createSenderWalletDB(0, 0);
    //auto binaryTreasury = createTreasury(senderWalletDB, kDefaultTestAmounts);
    auto binaryTreasury = createTreasury(senderWalletDB, testAmount);
    TestWalletRig sender("sender", senderWalletDB, completeAction, TestWalletRig::RegularWithoutPoWBbs);

    auto pushTxCreator = std::make_shared<lelantus::PushTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PushTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pushTxCreator));

    auto pullTxCreator = std::make_shared<lelantus::PullTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PullTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pullTxCreator));

    Node node;
    NodeObserver observer([&]()
    {
        auto cursor = node.get_Processor().m_Cursor;
        if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 3)
        {
            for (size_t i = 0; i < 500; i++)
            {
                wallet::TxParameters parameters(GenerateTxID());

                parameters.SetParameter(TxParameterID::TransactionType, TxType::PushTransaction)
                    .SetParameter(TxParameterID::Amount, 3800)
                    .SetParameter(TxParameterID::Fee, 1200)
                    .SetParameter(TxParameterID::MyID, sender.m_WalletID)
                    .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
                    .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
                    .SetParameter(TxParameterID::CreateTime, getTimestamp());

                sender.m_Wallet.StartTransaction(parameters);
            }
        }
        else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 4)
        {
            for (size_t i = 0; i < 500; i++)
            {
                wallet::TxParameters parameters(GenerateTxID());

                parameters.SetParameter(TxParameterID::TransactionType, TxType::PushTransaction)
                    .SetParameter(TxParameterID::Amount, 3800)
                    .SetParameter(TxParameterID::Fee, 1200)
                    .SetParameter(TxParameterID::MyID, sender.m_WalletID)
                    .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
                    .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
                    .SetParameter(TxParameterID::CreateTime, getTimestamp());

                sender.m_Wallet.StartTransaction(parameters);
            }
        }
        else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 5)
        {
            for (size_t i = 0; i < 500; i++)
            {
                wallet::TxParameters parameters(GenerateTxID());

                parameters.SetParameter(TxParameterID::TransactionType, TxType::PushTransaction)
                    .SetParameter(TxParameterID::Amount, 3800)
                    .SetParameter(TxParameterID::Fee, 1200)
                    .SetParameter(TxParameterID::MyID, sender.m_WalletID)
                    .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
                    .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
                    .SetParameter(TxParameterID::CreateTime, getTimestamp());

                sender.m_Wallet.StartTransaction(parameters);
            }
        }
        else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 6)
        {
            for (size_t i = 0; i < 500; i++)
            {
                wallet::TxParameters parameters(GenerateTxID());

                parameters.SetParameter(TxParameterID::TransactionType, TxType::PushTransaction)
                    .SetParameter(TxParameterID::Amount, 3800)
                    .SetParameter(TxParameterID::Fee, 1200)
                    .SetParameter(TxParameterID::MyID, sender.m_WalletID)
                    .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
                    .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
                    .SetParameter(TxParameterID::CreateTime, getTimestamp());

                sender.m_Wallet.StartTransaction(parameters);
            }
        }
        else if (cursor.m_Sid.m_Height == 150)
        {
            //WALLET_CHECK(completedCount == 0);
            mainReactor->stop();
        }
    });

    InitOwnNodeToTest(node, binaryTreasury, &observer, sender.m_WalletDB->get_MasterKdf(), 32125, 200);

    mainReactor->run();
}

void TestShortWindow()
{
    // save defaults
    ScopedGlobalRules rules;

    constexpr uint32_t kShieldedNMax = 64;
    constexpr uint32_t kShieldedNMin = 16;
    Rules::get().Shielded.NMax = kShieldedNMax;
    Rules::get().Shielded.NMin = kShieldedNMin;
    Rules::get().Shielded.MaxWindowBacklog = kShieldedNMax;

    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);

    constexpr size_t kCount = 200;
    constexpr size_t kSplitTxCount = 1;
    constexpr size_t kExtractShieldedTxCount = 4;

    int completedCount = kSplitTxCount + kCount + kExtractShieldedTxCount;
    auto completeAction = [&mainReactor, &completedCount](auto)
    {
        --completedCount;
        if (completedCount == 0)
        {
            mainReactor->stop();
        }
    };

    constexpr Amount kCoinAmount = 4000;
    constexpr Amount kFee = 2000;
    constexpr Amount kNominalCoin = kCoinAmount + kFee;
    AmountList testAmount(kCount, kNominalCoin);

    auto senderWalletDB = createSenderWalletDB(0, 0);
    // Coin for split TX
    auto binaryTreasury = createTreasury(senderWalletDB, { (kCount + 1) * kNominalCoin });
    TestWalletRig sender("sender", senderWalletDB, completeAction, TestWalletRig::RegularWithoutPoWBbs);

    auto pushTxCreator = std::make_shared<lelantus::PushTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PushTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pushTxCreator));

    auto pullTxCreator = std::make_shared<lelantus::PullTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PullTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pullTxCreator));

    Node node;
    NodeObserver observer([&]()
        {
            auto cursor = node.get_Processor().m_Cursor;
            // create 300(kCount) coins(split TX)
            if (cursor.m_Sid.m_Height == 3)
            {
                auto splitTxParameters = CreateSplitTransactionParameters(sender.m_WalletID, testAmount)
                    .SetParameter(TxParameterID::Fee, Amount(kNominalCoin));

                sender.m_Wallet.StartTransaction(splitTxParameters);
            }
            // insert 300(kCount) coins to shielded pool
            else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 3)
            {
                for (size_t i = 0; i < kCount; i++)
                {
                    auto parameters = lelantus::CreatePushTransactionParameters(sender.m_WalletID)
                        .SetParameter(TxParameterID::Amount, kCoinAmount)
                        .SetParameter(TxParameterID::Fee, kFee);

                    sender.m_Wallet.StartTransaction(parameters);
                }
            }
            // extract one of first shielded UTXO
            else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 15)
            {
                {
                    auto parameters = lelantus::CreatePullTransactionParameters(sender.m_WalletID)
                        .SetParameter(TxParameterID::Amount, kCoinAmount - kFee)
                        .SetParameter(TxParameterID::Fee, kFee)
                        .SetParameter(TxParameterID::ShieldedInputCfg, Lelantus::Cfg{ 4, 3 })
                        .SetParameter(TxParameterID::ShieldedInputMinCfg, Lelantus::Cfg{ 4, 2 })
                        .SetParameter(TxParameterID::ShieldedOutputId, 40U);

                    sender.m_Wallet.StartTransaction(parameters);
                }
                {
                    auto parameters = lelantus::CreatePullTransactionParameters(sender.m_WalletID)
                        .SetParameter(TxParameterID::Amount, kCoinAmount - kFee)
                        .SetParameter(TxParameterID::Fee, kFee)
                        .SetParameter(TxParameterID::ShieldedInputCfg, Lelantus::Cfg{ 4, 3 })
                        .SetParameter(TxParameterID::ShieldedInputMinCfg, Lelantus::Cfg{ 4, 2 })
                        .SetParameter(TxParameterID::ShieldedOutputId, 42U);

                    sender.m_Wallet.StartTransaction(parameters);
                }
                {
                    auto parameters = lelantus::CreatePullTransactionParameters(sender.m_WalletID)
                        .SetParameter(TxParameterID::Amount, kCoinAmount - kFee)
                        .SetParameter(TxParameterID::Fee, kFee)
                        .SetParameter(TxParameterID::ShieldedInputCfg, Lelantus::Cfg{ 4, 3 })
                        .SetParameter(TxParameterID::ShieldedInputMinCfg, Lelantus::Cfg{ 4, 2 })
                        .SetParameter(TxParameterID::ShieldedOutputId, 43U);

                    sender.m_Wallet.StartTransaction(parameters);
                }
            }
            else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 20)
            {
                auto parameters = lelantus::CreatePullTransactionParameters(sender.m_WalletID)
                    .SetParameter(TxParameterID::Amount, kCoinAmount - kFee)
                    .SetParameter(TxParameterID::Fee, kFee)
                    .SetParameter(TxParameterID::ShieldedInputCfg, Lelantus::Cfg{ 4, 3 })
                    .SetParameter(TxParameterID::ShieldedInputMinCfg, Lelantus::Cfg{ 4, 2 })
                    .SetParameter(TxParameterID::ShieldedOutputId, 62U);

                sender.m_Wallet.StartTransaction(parameters);
            }
            else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 30)
            {
                mainReactor->stop();
            }
        });

    InitOwnNodeToTest(node, binaryTreasury, &observer, sender.m_WalletDB->get_MasterKdf(), 32125, 200);

    mainReactor->run();

    auto txHistory = sender.m_WalletDB->getTxHistory(TxType::PullTransaction);

    WALLET_CHECK(completedCount == 0);
    WALLET_CHECK(txHistory.size() == kExtractShieldedTxCount);
    WALLET_CHECK(std::all_of(txHistory.begin(), txHistory.end(), [](const auto& tx) { return tx.m_status == TxStatus::Completed;}));
}

void TestShieldedUTXORollback()
{
    cout << "\nShielded UTXO rollback test\n";
    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);
    auto db = createSenderWalletDB();

    // create shieldedUTXO
    ShieldedCoin coin;
    Height height = 123123;
    Height spentHeight = height + 100;

    coin.m_confirmHeight = height;
    coin.m_spentHeight = spentHeight;

    db->saveShieldedCoin(coin);

    db->rollbackConfirmedShieldedUtxo(height - 100);

    auto coins = db->getShieldedCoins();
    WALLET_CHECK(coins.size() == 1);
    WALLET_CHECK(coins[0].m_spentHeight == MaxHeight);
    WALLET_CHECK(coins[0].m_confirmHeight == MaxHeight);
}

void TestPushTxRollbackByLowFee()
{
    cout << "\nTest rollback pushTx(reason: low fee).\n";
    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);

    auto completeAction = [&mainReactor](auto)
    {
        mainReactor->stop();        
    };

    auto senderWalletDB = createSenderWalletDB(0, 0);
    auto binaryTreasury = createTreasury(senderWalletDB, kDefaultTestAmounts);
    TestWalletRig sender("sender", senderWalletDB, completeAction, TestWalletRig::RegularWithoutPoWBbs);

    auto pushTxCreator = std::make_shared<lelantus::PushTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PushTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pushTxCreator));

    auto pullTxCreator = std::make_shared<lelantus::PullTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PullTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pullTxCreator));

    Node node;
    NodeObserver observer([&]()
        {
            auto cursor = node.get_Processor().m_Cursor;
            if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 3)
            {
                wallet::TxParameters parameters(GenerateTxID());

                parameters.SetParameter(TxParameterID::TransactionType, TxType::PushTransaction)
                    .SetParameter(TxParameterID::IsSender, true)
                    .SetParameter(TxParameterID::Amount, 3800)
                    .SetParameter(TxParameterID::Fee, 200)
                    .SetParameter(TxParameterID::MyID, sender.m_WalletID)
                    .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
                    .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
                    .SetParameter(TxParameterID::CreateTime, getTimestamp());

                sender.m_Wallet.StartTransaction(parameters);
            }
            else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 10)
            {
                mainReactor->stop();
            }
        });

    InitOwnNodeToTest(node, binaryTreasury, &observer, sender.m_WalletDB->get_MasterKdf(), 32125, 200);

    mainReactor->run();

    auto txHistory = sender.m_WalletDB->getTxHistory(TxType::PushTransaction);
    // check Tx params
    WALLET_CHECK(txHistory.size() == 1);
    WALLET_CHECK(txHistory[0].m_status == TxStatus::Failed);
    WALLET_CHECK(*txHistory[0].GetParameter<uint8_t>(TxParameterID::TransactionRegistered) == proto::TxStatus::LowFee);
    // check coins
    auto coins = sender.m_WalletDB->getCoinsByTx(*txHistory[0].GetTxID());
    WALLET_CHECK(coins.size() == 0);    
    auto shieldedCoins = sender.m_WalletDB->getShieldedCoins();
    WALLET_CHECK(shieldedCoins.size() == 0);
}

void TestPullTxRollbackByLowFee()
{
    cout << "\nTest rollback pullTx(reason: low fee).\n";
    io::Reactor::Ptr mainReactor{ io::Reactor::create() };
    io::Reactor::Scope scope(*mainReactor);

    int completedCount = 2;
    auto completeAction = [&mainReactor, &completedCount](auto)
    {
        --completedCount;
        if (completedCount == 0)
        {
            mainReactor->stop();
        }
    };

    auto senderWalletDB = createSenderWalletDB(0, 0);
    auto binaryTreasury = createTreasury(senderWalletDB, kDefaultTestAmounts);
    TestWalletRig sender("sender", senderWalletDB, completeAction, TestWalletRig::RegularWithoutPoWBbs);

    auto pushTxCreator = std::make_shared<lelantus::PushTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PushTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pushTxCreator));

    auto pullTxCreator = std::make_shared<lelantus::PullTransaction::Creator>();
    sender.m_Wallet.RegisterTransactionType(TxType::PullTransaction, std::static_pointer_cast<BaseTransaction::Creator>(pullTxCreator));

    TxID pullTxID = {};
    Node node;
    NodeObserver observer([&]()
        {
            auto cursor = node.get_Processor().m_Cursor;
            if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 3)
            {
                wallet::TxParameters parameters(GenerateTxID());

                parameters.SetParameter(TxParameterID::TransactionType, TxType::PushTransaction)
                    .SetParameter(TxParameterID::IsSender, true)
                    .SetParameter(TxParameterID::Amount, 3800)
                    .SetParameter(TxParameterID::Fee, 1200)
                    .SetParameter(TxParameterID::MyID, sender.m_WalletID)
                    .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
                    .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
                    .SetParameter(TxParameterID::CreateTime, getTimestamp());

                sender.m_Wallet.StartTransaction(parameters);
            }
            else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 10)
            {
                wallet::TxParameters parameters(GenerateTxID());

                parameters.SetParameter(TxParameterID::TransactionType, TxType::PullTransaction)
                    .SetParameter(TxParameterID::IsSender, false)
                    .SetParameter(TxParameterID::Amount, 3600)
                    .SetParameter(TxParameterID::Fee, 200)
                    .SetParameter(TxParameterID::MyID, sender.m_WalletID)
                    .SetParameter(TxParameterID::Lifetime, kDefaultTxLifetime)
                    .SetParameter(TxParameterID::PeerResponseTime, kDefaultTxResponseTime)
                    .SetParameter(TxParameterID::WindowBegin, 0U)
                    .SetParameter(TxParameterID::ShieldedInputCfg, Lelantus::Cfg{})
                    .SetParameter(TxParameterID::ShieldedOutputId, 0U)
                    .SetParameter(TxParameterID::CreateTime, getTimestamp());

                pullTxID = sender.m_Wallet.StartTransaction(parameters);
            }
            else if (cursor.m_Sid.m_Height == Rules::get().pForks[2].m_Height + 20)
            {
                mainReactor->stop();
            }
        });

    InitOwnNodeToTest(node, binaryTreasury, &observer, sender.m_WalletDB->get_MasterKdf(), 32125, 200);

    mainReactor->run();

    WALLET_CHECK(completedCount == 0);
    auto txHistory = sender.m_WalletDB->getTxHistory(TxType::PullTransaction);
    // check Tx params
    WALLET_CHECK(txHistory.size() == 1);
    WALLET_CHECK(txHistory[0].m_status == TxStatus::Failed);
    WALLET_CHECK(*txHistory[0].GetParameter<uint8_t>(TxParameterID::TransactionRegistered) == proto::TxStatus::LowFee);
    // check coins
    auto coins = sender.m_WalletDB->getCoinsByTx(pullTxID);
    WALLET_CHECK(coins.size() == 0);
    auto shieldedCoins = sender.m_WalletDB->getShieldedCoins();
    WALLET_CHECK(shieldedCoins.size() == 1);
    WALLET_CHECK(!shieldedCoins[0].m_spentTxId.is_initialized());
}

int main()
{
    int logLevel = LOG_LEVEL_DEBUG;
    auto logger = beam::Logger::create(logLevel, logLevel);
    Rules::get().FakePoW = true;
    Rules::get().UpdateChecksum();
    Height fork1Height = 10;
    Height fork2Height = 20;
    Rules::get().pForks[1].m_Height = fork1Height;
    Rules::get().pForks[2].m_Height = fork2Height;

    TestSimpleTx();

    /*TestManyTransactons();*/

    TestShortWindow();

    TestShieldedUTXORollback();
    TestPushTxRollbackByLowFee();
    TestPullTxRollbackByLowFee();

    assert(g_failureCount == 0);
    return WALLET_CHECK_RESULT;
}