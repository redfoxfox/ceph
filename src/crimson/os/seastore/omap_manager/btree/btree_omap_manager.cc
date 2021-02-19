// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <string.h>

#include "crimson/common/log.h"

#include "crimson/os/seastore/seastore_types.h"
#include "crimson/os/seastore/omap_manager/btree/btree_omap_manager.h"
#include "crimson/os/seastore/omap_manager/btree/omap_btree_node_impl.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_filestore);
  }
}

namespace crimson::os::seastore::omap_manager {

BtreeOMapManager::BtreeOMapManager(
  TransactionManager &tm)
  : tm(tm) {}

BtreeOMapManager::initialize_omap_ret
BtreeOMapManager::initialize_omap(Transaction &t)
{

  logger().debug("{}", __func__);
  return tm.alloc_extent<OMapLeafNode>(t, L_ADDR_MIN, OMAP_BLOCK_SIZE)
    .safe_then([](auto&& root_extent) {
      root_extent->set_size(0);
      omap_node_meta_t meta{1};
      root_extent->set_meta(meta);
      omap_root_t omap_root = omap_root_t(1, root_extent->get_laddr());
      return initialize_omap_ertr::make_ready_future<omap_root_t>(omap_root);
  });
}

BtreeOMapManager::get_root_ret
BtreeOMapManager::get_omap_root(const omap_root_t &omap_root, Transaction &t)
{
  assert(omap_root.omap_root_laddr != L_ADDR_NULL);
  laddr_t laddr = omap_root.omap_root_laddr;
  return omap_load_extent(get_omap_context(t), laddr, omap_root.depth);
}

BtreeOMapManager::handle_root_split_ret
BtreeOMapManager::handle_root_split(omap_root_t &omap_root, omap_context_t oc,
                                    OMapNode::mutation_result_t mresult)
{
  return oc.tm.alloc_extent<OMapInnerNode>(oc.t, L_ADDR_MIN, OMAP_BLOCK_SIZE)
    .safe_then([&omap_root, mresult](auto&& nroot) {
    auto [left, right, pivot] = *(mresult.split_tuple);
    omap_node_meta_t meta{omap_root.depth + 1};
    nroot->set_meta(meta);
    nroot->journal_inner_insert(nroot->iter_begin(), left->get_laddr(),
                                "", nroot->maybe_get_delta_buffer());
    nroot->journal_inner_insert(nroot->iter_begin() + 1, right->get_laddr(),
                                pivot, nroot->maybe_get_delta_buffer());
    omap_root.omap_root_laddr = nroot->get_laddr();
    omap_root.depth += 1;
    omap_root.state = omap_root_state_t::MUTATED;
    return handle_root_split_ertr::make_ready_future<bool>(true);
  });
}

BtreeOMapManager::handle_root_merge_ret
BtreeOMapManager::handle_root_merge(omap_root_t &omap_root, omap_context_t oc,
                                    OMapNode::mutation_result_t mresult)
{
  auto root = *(mresult.need_merge);
  auto iter = root->cast<OMapInnerNode>()->iter_begin();
  omap_root.omap_root_laddr = iter->get_node_key().laddr;
  omap_root.depth -= 1;
  omap_root.state = omap_root_state_t::MUTATED;
  return oc.tm.dec_ref(oc.t, root->get_laddr()
  ).safe_then([] (auto &&ret) {
    return handle_root_merge_ertr::make_ready_future<bool>(true);
  }).handle_error(
    handle_root_merge_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in handle_root_merge"
    }
  );
}


BtreeOMapManager::omap_get_value_ret
BtreeOMapManager::omap_get_value(const omap_root_t &omap_root, Transaction &t,
                                 const std::string &key)
{
  logger().debug("{}: {}", __func__, key);
  return get_omap_root(omap_root, t).safe_then([this, &t, &key](auto&& extent) {
    return extent->get_value(get_omap_context(t), key);
  }).safe_then([](auto &&e) {
    logger().debug("{}: {} -> {}", __func__, e.first, e.second);
    return omap_get_value_ret(
        omap_get_value_ertr::ready_future_marker{},
        std::move(e));
  });
}

BtreeOMapManager::omap_set_key_ret
BtreeOMapManager::omap_set_key(omap_root_t &omap_root, Transaction &t,
                             const std::string &key, const std::string &value)
{
  logger().debug("{}: {} -> {}", __func__, key, value);
  return get_omap_root(omap_root, t).safe_then([this, &t, &key, &value](auto root) {
    return root->insert(get_omap_context(t), key, value);
  }).safe_then([this, &omap_root, &t](auto mresult) {
    if (mresult.status == mutation_status_t::SUCCESS)
      return omap_set_key_ertr::make_ready_future<bool>(true);
    else if (mresult.status == mutation_status_t::WAS_SPLIT)
      return handle_root_split(omap_root, get_omap_context(t), mresult);
    else
      return omap_set_key_ertr::make_ready_future<bool>(false);
  });
}

BtreeOMapManager::omap_rm_key_ret
BtreeOMapManager::omap_rm_key(omap_root_t &omap_root, Transaction &t, const std::string &key)
{
  logger().debug("{}: {}", __func__, key);
  return get_omap_root(omap_root, t).safe_then([this, &t, &key](auto root) {
    return root->rm_key(get_omap_context(t), key);
  }).safe_then([this, &omap_root, &t](auto mresult) {
    if (mresult.status == mutation_status_t::SUCCESS)
      return omap_rm_key_ertr::make_ready_future<bool>(true);
    else if (mresult.status == mutation_status_t::WAS_SPLIT)
      return handle_root_split(omap_root, get_omap_context(t), mresult);
    else if (mresult.status == mutation_status_t::NEED_MERGE) {
      auto root = *(mresult.need_merge);
      if (root->get_node_size() == 1 && omap_root.depth != 1)
        return handle_root_merge(omap_root, get_omap_context(t), mresult);
      else
        return omap_rm_key_ertr::make_ready_future<bool>(true);
    }
    else
      return omap_rm_key_ertr::make_ready_future<bool>(false);
  });

}

BtreeOMapManager::omap_list_keys_ret
BtreeOMapManager::omap_list_keys(const omap_root_t &omap_root, Transaction &t,
                                 std::string &start, size_t max_result_size)
{
  logger().debug("{}", __func__);
  return get_omap_root(omap_root, t).safe_then([this, &t, &start,
    max_result_size] (auto extent) {
    return extent->list_keys(get_omap_context(t), start, max_result_size)
      .safe_then([](auto &&result) {
      return omap_list_keys_ret(
             omap_list_keys_ertr::ready_future_marker{},
             std::move(result));
    });
  });

}

BtreeOMapManager::omap_list_ret
BtreeOMapManager::omap_list(const omap_root_t &omap_root, Transaction &t,
                            std::string &start, size_t max_result_size)
{
  logger().debug("{}", __func__);
  return get_omap_root(omap_root, t).safe_then([this, &t, &start, max_result_size]
    (auto extent) {
    return extent->list(get_omap_context(t), start, max_result_size)
      .safe_then([](auto &&result) {
      return omap_list_ret(
             omap_list_ertr::ready_future_marker{},
             std::move(result));
    });
  });
}

BtreeOMapManager::omap_clear_ret
BtreeOMapManager::omap_clear(omap_root_t &omap_root, Transaction &t)
{
  logger().debug("{}", __func__);
  return get_omap_root(omap_root, t).safe_then([this, &t](auto extent) {
    return extent->clear(get_omap_context(t));
  }).safe_then([this, &omap_root, &t] {
    return tm.dec_ref(t, omap_root.omap_root_laddr).safe_then([&omap_root] (auto ret) {
      omap_root.state = omap_root_state_t::MUTATED;
      omap_root.depth = 0;
      omap_root.omap_root_laddr = L_ADDR_NULL;
      return omap_clear_ertr::now();
    });
  }).handle_error(
    omap_clear_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in BtreeOMapManager::omap_clear"
    }
  );
}

}
