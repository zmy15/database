#include <gtest/gtest.h>
#include "storage/table_page.h"
#include "storage/tuple.h"
#include "storage/file_disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include <memory>

using namespace db;

class TablePageTest : public ::testing::Test {
protected:
    void SetUp() override {
        disk_mgr_ = std::make_unique<FileDiskManager>("test_tablepage.db");
        bpm_ = std::make_unique<BufferPoolManager>(16, disk_mgr_.get());
        Page* page = bpm_->NewPage(&page_id_);
        ASSERT_NE(page, nullptr);
        table_page_ = reinterpret_cast<TablePage*>(page);
        table_page_->Init(page_id_, INVALID_PAGE_ID, bpm_.get());
    }

    void TearDown() override {
        if (bpm_) {
            bpm_->UnpinPage(page_id_, false);
            bpm_->Destroy();
        }
        bpm_.reset();
        disk_mgr_.reset();
        std::remove("test_tablepage.db");
    }

    static Tuple MakeTuple(const std::vector<std::string>& vals) {
        return Tuple(vals);
    }

    std::unique_ptr<FileDiskManager> disk_mgr_;
    std::unique_ptr<BufferPoolManager> bpm_;
    TablePage* table_page_ = nullptr;
    page_id_t page_id_ = INVALID_PAGE_ID;
};

TEST_F(TablePageTest, InitSetsMetadata) {
    EXPECT_EQ(table_page_->GetTablePageId(), page_id_);
    EXPECT_EQ(table_page_->GetPrevPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(table_page_->GetNextPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(table_page_->GetTupleCount(), 0u);
}

TEST_F(TablePageTest, InsertSingleTuple) {
    Tuple t = MakeTuple({"Alice", "30"});
    RID rid;
    EXPECT_TRUE(table_page_->InsertTuple(t, &rid));
    EXPECT_EQ(table_page_->GetTupleCount(), 1u);
    EXPECT_EQ(rid.GetPageId(), page_id_);
    EXPECT_EQ(rid.GetSlotNum(), 0u);

    auto result = table_page_->GetTuple(rid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[0], "Alice");
    EXPECT_EQ(result->GetValues()[1], "30");
}

TEST_F(TablePageTest, InsertMultipleTuples) {
    const int N = 10;
    std::vector<RID> rids;
    for (int i = 0; i < N; ++i) {
        Tuple t = MakeTuple({"Name_" + std::to_string(i), std::to_string(i * 10)});
        RID rid;
        EXPECT_TRUE(table_page_->InsertTuple(t, &rid));
        rids.push_back(rid);
    }
    EXPECT_EQ(table_page_->GetTupleCount(), static_cast<uint32_t>(N));
    for (int i = 0; i < N; ++i) {
        auto result = table_page_->GetTuple(rids[i]);
        ASSERT_TRUE(result.has_value()) << "Missing tuple at slot " << i;
        EXPECT_EQ(result->GetValues()[0], "Name_" + std::to_string(i));
        EXPECT_EQ(result->GetValues()[1], std::to_string(i * 10));
    }
}

TEST_F(TablePageTest, MarkDeleteRemovesTuple) {
    Tuple t = MakeTuple({"Bob", "25"});
    RID rid;
    table_page_->InsertTuple(t, &rid);
    EXPECT_TRUE(table_page_->GetTuple(rid).has_value());
    EXPECT_TRUE(table_page_->MarkDelete(rid.GetSlotNum()));
    EXPECT_FALSE(table_page_->GetTuple(rid).has_value());
}

TEST_F(TablePageTest, UpdateTupleInPlace) {
    Tuple t1 = MakeTuple({"OldName", "OldVal"});
    RID rid;
    table_page_->InsertTuple(t1, &rid);
    Tuple t2 = MakeTuple({"NewName", "NewVal"});
    EXPECT_TRUE(table_page_->UpdateTuple(rid.GetSlotNum(), t2));
    auto result = table_page_->GetTuple(rid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[0], "NewName");
    EXPECT_EQ(result->GetValues()[1], "NewVal");
}

TEST_F(TablePageTest, UpdateTupleDifferentSizeFails) {
    Tuple t1 = MakeTuple({"Short"});
    RID rid;
    table_page_->InsertTuple(t1, &rid);
    Tuple t2 = MakeTuple({"LongerName", "ExtraField"});
    EXPECT_FALSE(table_page_->UpdateTuple(rid.GetSlotNum(), t2));
    auto result = table_page_->GetTuple(rid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[0], "Short");
}

TEST_F(TablePageTest, InvalidRIDReturnsNullopt) {
    Tuple t = MakeTuple({"Test"});
    RID rid;
    table_page_->InsertTuple(t, &rid);
    RID invalid_rid(page_id_, 100);
    EXPECT_FALSE(table_page_->GetTuple(invalid_rid).has_value());
    RID wrong_page(9999, 0);
    EXPECT_FALSE(table_page_->GetTuple(wrong_page).has_value());
}

TEST_F(TablePageTest, LargeValueInsertAndRead) {
    std::string big_data(500, 'X');
    Tuple t = MakeTuple({"big_key", big_data});
    RID rid;
    EXPECT_TRUE(table_page_->InsertTuple(t, &rid));
    auto result = table_page_->GetTuple(rid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->GetValues()[1], big_data);
}

TEST_F(TablePageTest, PrevNextPagePointers) {
    EXPECT_EQ(table_page_->GetPrevPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(table_page_->GetNextPageId(), INVALID_PAGE_ID);
    table_page_->SetNextPageId(42);
    EXPECT_EQ(table_page_->GetNextPageId(), 42);
    table_page_->SetPrevPageId(10);
    EXPECT_EQ(table_page_->GetPrevPageId(), 10);
}

TEST_F(TablePageTest, GetTupleNonExistentSlot) {
    RID rid(page_id_, 0);
    EXPECT_FALSE(table_page_->GetTuple(rid).has_value());
    Tuple t = MakeTuple({"First"});
    table_page_->InsertTuple(t, &rid);
    RID rid2(page_id_, 1);
    EXPECT_FALSE(table_page_->GetTuple(rid2).has_value());
}
