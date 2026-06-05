#include "index/b_plus_tree.h"
#include "index/b_plus_tree_page.h"
#include <iostream>
#include <stack>

namespace db {

// ============================================================
// 辅助：在内部节点中根据 key 选择子节点
// ============================================================

static page_id_t FindChildInNode(BPlusTreePage* node, const std::string& key) {
    uint32_t count = node->GetSlotCount();
    if (count == 0) {
        return node->GetPrevPageId();
    }
    // 如果 key < 最小键，走 child[0]（即 prev_page_id）
    if (key < node->GetKey(0)) {
        return node->GetPrevPageId();
    }
    // 否则找到最后一个 key[i] <= key 的 slot，走 child[i+1]
    int lo = 0, hi = static_cast<int>(count) - 1;
    int pos = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (node->GetKey(mid) <= key) {
            pos = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return node->GetChildId(pos);
}

// ============================================================
// 点查：GetValue
// ============================================================

std::optional<Tuple> BPlusTree::GetValue(const std::string& key, txn_id_t txn_id) {
    (void)txn_id; // 当前简化实现不使用事务 ID

    if (root_page_id_ == INVALID_PAGE_ID) {
        return std::nullopt;
    }

    root_latch_.lock_shared();
    page_id_t page_id = root_page_id_;

    // 从根向下遍历到叶子
    while (true) {
        Page* page = bpm_->FetchPage(page_id);
        if (!page) {
            root_latch_.unlock_shared();
            return std::nullopt;
        }
        auto* node = reinterpret_cast<BPlusTreePage*>(page);

        if (node->IsLeaf()) {
            int idx = node->FindKey(key);
            std::optional<Tuple> result;
            if (idx >= 0) {
                result = node->GetValue(idx);
            }
            bpm_->UnpinPage(page_id, false);
            root_latch_.unlock_shared();
            return result;
        }

        // 内部节点：找下一个子节点
        page_id_t next_page = FindChildInNode(node, key);
        bpm_->UnpinPage(page_id, false);
        page_id = next_page;
    }
}

// ============================================================
// 删除：Remove
// ============================================================

bool BPlusTree::Remove(const std::string& key) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return false;
    }

    root_latch_.lock();

    std::stack<std::pair<page_id_t, BPlusTreePage*>> path;
    page_id_t page_id = root_page_id_;

    while (true) {
        Page* page = bpm_->FetchPage(page_id);
        if (!page) {
            root_latch_.unlock();
            while (!path.empty()) {
                auto& p = path.top();
                bpm_->UnpinPage(p.first, false);
                path.pop();
            }
            return false;
        }
        auto* node = reinterpret_cast<BPlusTreePage*>(page);

        if (node->IsLeaf()) {
            int idx = node->FindKey(key);
            bool removed = false;
            if (idx >= 0) {
                node->MarkSlotDeleted(idx);
                removed = true;
            }
            bpm_->UnpinPage(page_id, removed);
            while (!path.empty()) {
                auto& p = path.top();
                bpm_->UnpinPage(p.first, false);
                path.pop();
            }
            root_latch_.unlock();
            return removed;
        }

        page_id_t next_page = FindChildInNode(node, key);
        path.push({page_id, node});
        page_id = next_page;
    }
}

// ============================================================
// 插入：Insert
// ============================================================

bool BPlusTree::Insert(const std::string& key, const Tuple& value, txn_id_t txn_id) {
    (void)txn_id;

    // 空树：直接创建根节点
    if (root_page_id_ == INVALID_PAGE_ID) {
        return InsertIntoEmptyTree(key, value);
    }

    root_latch_.lock();

    // 从根向下查找，记录路径（用于后续分裂时回溯）
    std::stack<std::pair<page_id_t, BPlusTreePage*>> path;
    page_id_t page_id = root_page_id_;

    while (true) {
        Page* page = bpm_->FetchPage(page_id);
        if (!page) {
            root_latch_.unlock();
            // 释放路径中已持有的页面
            while (!path.empty()) {
                auto& [pid, pnode] = path.top();
                bpm_->UnpinPage(pid, false);
                path.pop();
            }
            return false;
        }
        auto* node = reinterpret_cast<BPlusTreePage*>(page);

        if (node->IsLeaf()) {
            // 到达叶子，尝试插入
            int pos = node->FindInsertPos(key);
            bool inserted = node->InsertSlot(pos, key, value);

            if (inserted) {
                // 插入成功，沿路径释放所有页面
                bpm_->UnpinPage(page_id, true);
                while (!path.empty()) {
                    auto& [pid, pnode] = path.top();
                    bpm_->UnpinPage(pid, false);
                    path.pop();
                }
                root_latch_.unlock();
                return true;
            }

            // 叶子已满，需要分裂
            // 创建新页面
            page_id_t new_page_id;
            Page* new_page = bpm_->NewPage(&new_page_id);
            if (!new_page) {
                bpm_->UnpinPage(page_id, false);
                while (!path.empty()) {
                    auto& [pid, pnode] = path.top();
                    bpm_->UnpinPage(pid, false);
                    path.pop();
                }
                root_latch_.unlock();
                return false;
            }
            auto* new_node = reinterpret_cast<BPlusTreePage*>(new_page);
            new_node->Init(new_page_id, true);

            // 先分裂：将一半数据移到新节点（避免 force 插入破坏槽位区）
            uint32_t old_count = node->GetSlotCount();
            node->MoveHalfTo(new_node);

            // 确定新数据应插入哪个页面
            uint32_t new_mid = node->GetSlotCount(); // 分裂后原页面的 slot 数
            // 注意：pos == new_mid 表示应插入到新页面的第一个位置（slot[mid] 已被移走）
            if (static_cast<uint32_t>(pos) < new_mid) {
                // 插入位置在原页面
                node->InsertSlot(pos, key, value);
            } else {
                // 插入位置在新页面
                uint32_t new_pos = static_cast<uint32_t>(pos) - new_mid;
                new_node->InsertSlot(new_pos, key, value);
            }

            // 获取分裂键（新页面的第一个 key）
            std::string split_key = new_node->GetKey(0);

            // 释放当前叶子和新叶子
            bpm_->UnpinPage(new_page_id, true);
            bpm_->UnpinPage(page_id, true);

            // 向上插入父节点
            root_latch_.unlock();
            InsertIntoParent(page_id, split_key, new_page_id);
            return true;
        }

        // 内部节点：继续向下
        page_id_t next_page = FindChildInNode(node, key);
        path.push({page_id, node}); // 暂不 unpin，保留在路径中
        page_id = next_page;
    }
}

// ============================================================
// InsertIntoEmptyTree
// ============================================================

bool BPlusTree::InsertIntoEmptyTree(const std::string& key, const Tuple& value) {
    page_id_t new_page_id;
    Page* page = bpm_->NewPage(&new_page_id);
    if (!page) return false;

    auto* root = reinterpret_cast<BPlusTreePage*>(page);
    root->Init(new_page_id, true);
    root->InsertSlot(0, key, value);

    root_page_id_ = new_page_id;
    bpm_->UnpinPage(new_page_id, true);
    return true;
}

// ============================================================
// InsertIntoParent — 处理分裂后的父节点更新
// ============================================================

bool BPlusTree::InsertIntoParent(page_id_t old_page_id, const std::string& split_key,
                                  page_id_t new_page_id) {
    // 如果 old_page_id 是根，需要创建新的根
    if (old_page_id == root_page_id_) {
        page_id_t new_root_id;
        Page* page = bpm_->NewPage(&new_root_id);
        if (!page) return false;

        auto* new_root = reinterpret_cast<BPlusTreePage*>(page);
        new_root->Init(new_root_id, false); // 内部节点

        // 设置 prev_page_id = old_page_id（第一个孩子）
        new_root->SetPrevPageId(old_page_id);
        // 插入 (split_key, new_page_id)
        new_root->InsertKeyAt(0, split_key, new_page_id);

        root_page_id_ = new_root_id;
        bpm_->UnpinPage(new_root_id, true);

        // 更新两个子节点的父指针（B+树中通过 UpdateParentPointers 维护）
        UpdateParentPointers(old_page_id, new_root);
        UpdateParentPointers(new_page_id, new_root);

        return true;
    }

    // 否则需要找到 old_page_id 的父节点，在其中插入分裂键
    // 递归从根向下查找父节点
    root_latch_.lock();

    std::stack<std::pair<page_id_t, BPlusTreePage*>> path;
    page_id_t page_id = root_page_id_;

    while (true) {
        Page* page = bpm_->FetchPage(page_id);
        if (!page) {
            root_latch_.unlock();
            return false;
        }
        auto* node = reinterpret_cast<BPlusTreePage*>(page);

        if (node->IsLeaf()) {
            // 不应该到达叶子（old_page_id 应该在更上层）
            bpm_->UnpinPage(page_id, false);
            while (!path.empty()) {
                auto& [pid, pnode] = path.top();
                bpm_->UnpinPage(pid, false);
                path.pop();
            }
            root_latch_.unlock();
            return false;
        }

        // 检查这个节点的子节点是否包含 old_page_id
        bool found = false;
        page_id_t next_page = INVALID_PAGE_ID;

        if (node->GetPrevPageId() == old_page_id) {
            found = true;
        }
        uint32_t count = node->GetSlotCount();
        for (uint32_t i = 0; i < count; ++i) {
            if (node->GetChildId(i) == old_page_id) {
                found = true;
                break;
            }
        }

        if (found) {
            // 找到了父节点，尝试插入
            // 找到 split_key 应该插入的位置
            int pos = node->FindInsertPos(split_key);
            bool inserted = node->InsertKeyAt(pos, split_key, new_page_id);

            if (inserted) {
                // 插入成功
                bpm_->UnpinPage(page_id, true);
                while (!path.empty()) {
                    auto& [pid, pnode] = path.top();
                    bpm_->UnpinPage(pid, false);
                    path.pop();
                }
                root_latch_.unlock();
                UpdateParentPointers(new_page_id, node);
                return true;
            }

            // 父节点也满了，继续分裂
            page_id_t new_parent_id;
            Page* new_parent_page = bpm_->NewPage(&new_parent_id);
            if (!new_parent_page) {
                bpm_->UnpinPage(page_id, false);
                while (!path.empty()) {
                    auto& [pid, pnode] = path.top();
                    bpm_->UnpinPage(pid, false);
                    path.pop();
                }
                root_latch_.unlock();
                return false;
            }
            auto* new_parent = reinterpret_cast<BPlusTreePage*>(new_parent_page);
            new_parent->Init(new_parent_id, false);

            // 先分裂：将一半数据移到新节点（避免 force 插入破坏槽位区）
            uint32_t old_parent_count = node->GetSlotCount();
            node->MoveHalfTo(new_parent);

            // 确定新键应插入哪个页面
            uint32_t new_parent_mid = node->GetSlotCount(); // 分裂后原页面的 slot 数
            // 注意：pos == new_parent_mid 表示应插入到新父节点的第一个位置
            if (static_cast<uint32_t>(pos) < new_parent_mid) {
                // 插入位置在原页面
                node->InsertKeyAt(pos, split_key, new_page_id);
            } else {
                // 插入位置在新页面
                uint32_t new_pos = static_cast<uint32_t>(pos) - new_parent_mid;
                new_parent->InsertKeyAt(new_pos, split_key, new_page_id);
            }

            std::string parent_split_key = new_parent->GetKey(0);

            bpm_->UnpinPage(new_parent_id, true);
            bpm_->UnpinPage(page_id, true);
            while (!path.empty()) {
                auto& [pid, pnode] = path.top();
                bpm_->UnpinPage(pid, false);
                path.pop();
            }
            root_latch_.unlock();

            // 递归向上
            UpdateParentPointers(new_page_id, new_parent);
            return InsertIntoParent(page_id, parent_split_key, new_parent_id);
        }

        // 还没找到父节点，继续向下
        // 根据 split_key 判断走哪个子节点（old_page_id 可能在任意分支）
        // 安全做法：遍历所有子节点来找到 old_page_id
        // 先检查 prev_page_id
        if (node->GetPrevPageId() != INVALID_PAGE_ID && node->GetPrevPageId() != old_page_id) {
            next_page = node->GetPrevPageId();
        }
        // 遍历 slot 的子节点
        for (uint32_t i = 0; i < count && next_page == INVALID_PAGE_ID; ++i) {
            page_id_t child = node->GetChildId(i);
            if (child != INVALID_PAGE_ID && child != old_page_id) {
                // 用 split_key 来导航
                if (split_key >= node->GetKey(i) || i == count - 1) {
                    next_page = child;
                }
            }
        }
        // 简单策略：用 split_key 导航
        next_page = FindChildInNode(node, split_key);

        path.push({page_id, node});
        page_id = next_page;
    }
}

// ============================================================
// UpdateParentPointers — 维护内部节点中子节点页面指向（B+树中此项为空操作，
// 因为我们不维护子节点到父节点的反向指针，仅保留接口兼容）
// ============================================================

void BPlusTree::UpdateParentPointers(page_id_t page_id, BPlusTreePage* node) {
    (void)page_id;
    (void)node;
    // 在当前简化实现中，不维护 parent 指针
    // 分裂时通过递归 InsertIntoParent 向上传播
}

// ============================================================
// 范围扫描：ScanRange
// ============================================================

std::vector<Tuple> BPlusTree::ScanRange(const std::string& start_key,
                                         const std::string& end_key,
                                         txn_id_t txn_id) {
    (void)txn_id;
    std::vector<Tuple> results;

    if (root_page_id_ == INVALID_PAGE_ID) {
        return results;
    }

    root_latch_.lock_shared();
    page_id_t page_id = root_page_id_;

    // 从根向下遍历到包含 start_key 的叶子
    while (true) {
        Page* page = bpm_->FetchPage(page_id);
        if (!page) {
            root_latch_.unlock_shared();
            return results;
        }
        auto* node = reinterpret_cast<BPlusTreePage*>(page);

        if (node->IsLeaf()) {
            // 找到叶子，开始扫描
            page_id_t leaf_id = page_id;
            while (leaf_id != INVALID_PAGE_ID) {
                Page* leaf_page = bpm_->FetchPage(leaf_id);
                if (!leaf_page) break;
                auto* leaf = reinterpret_cast<BPlusTreePage*>(leaf_page);

                uint32_t slot_count = leaf->GetSlotCount();
                for (uint32_t i = 0; i < slot_count; ++i) {
                    if (leaf->IsSlotDeleted(i)) continue;
                    std::string key = leaf->GetKey(i);
                    // 在范围内的判断
                    if (key >= start_key && key <= end_key) {
                        results.push_back(leaf->GetValue(i));
                    }
                    // 如果已经超过 end_key，提前终止
                    if (key > end_key) {
                        bpm_->UnpinPage(leaf_id, false);
                        bpm_->UnpinPage(page_id, false);
                        root_latch_.unlock_shared();
                        return results;
                    }
                }

                page_id_t next = leaf->GetNextPageId();
                bpm_->UnpinPage(leaf_id, false);
                leaf_id = next;
            }

            bpm_->UnpinPage(page_id, false);
            root_latch_.unlock_shared();
            return results;
        }

        // 内部节点：找下一个子节点
        page_id_t next_page = FindChildInNode(node, start_key);
        bpm_->UnpinPage(page_id, false);
        page_id = next_page;
    }
}

// ============================================================
// 收集所有页面 ID（递归）
// ============================================================

void BPlusTree::CollectAllPageIds(page_id_t page_id, std::vector<page_id_t>& ids) {
    if (page_id == INVALID_PAGE_ID) return;

    Page* page = bpm_->FetchPage(page_id);
    if (!page) return;

    auto* node = reinterpret_cast<BPlusTreePage*>(page);
    ids.push_back(page_id);

    if (!node->IsLeaf()) {
        // 内部节点：收集 child[0]（prev_page_id）和所有 slot 中的子节点
        page_id_t child0 = node->GetPrevPageId();
        bpm_->UnpinPage(page_id, false);
        CollectAllPageIds(child0, ids);
        // 重新 fetch 以读取 slot（因为 Unpin 后页面可能被淘汰）
        // 实际上在同一个调用栈帧中 page 不会被淘汰（pin_count > 0）
        // 上面的 Unpin 会减少 pin_count，但 CollectAllPageIds 在 Unpin 后调用，
        // 所以这里需要重新获取。更安全的做法是先收集所有子节点 ID 再递归。
    } else {
        bpm_->UnpinPage(page_id, false);
    }
}

// ============================================================
// 删除整棵 B+ 树：Drop
// ============================================================

void BPlusTree::Drop() {
    if (root_page_id_ == INVALID_PAGE_ID) return;

    root_latch_.lock();

    // 使用栈进行迭代式 BFS/DFS 遍历，避免递归 unpin 问题
    std::vector<page_id_t> all_pages;
    std::vector<page_id_t> stack;
    stack.push_back(root_page_id_);

    while (!stack.empty()) {
        page_id_t pid = stack.back();
        stack.pop_back();

        Page* page = bpm_->FetchPage(pid);
        if (!page) continue;

        auto* node = reinterpret_cast<BPlusTreePage*>(page);
        all_pages.push_back(pid);

        if (!node->IsLeaf()) {
            // 内部节点：收集子节点
            uint32_t slot_count = node->GetSlotCount();
            for (uint32_t i = 0; i < slot_count; ++i) {
                page_id_t child = node->GetChildId(i);
                if (child != INVALID_PAGE_ID) {
                    stack.push_back(child);
                }
            }
            // child[0]（prev_page_id）
            page_id_t child0 = node->GetPrevPageId();
            if (child0 != INVALID_PAGE_ID) {
                stack.push_back(child0);
            }
        }

        bpm_->UnpinPage(pid, false);
    }

    // 删除所有页面
    for (page_id_t pid : all_pages) {
        bpm_->DeletePage(pid);
    }

    root_page_id_ = INVALID_PAGE_ID;
    root_latch_.unlock();
}

} // namespace db
