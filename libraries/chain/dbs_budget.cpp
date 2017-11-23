#include <scorum/chain/dbs_budget.hpp>
#include <scorum/chain/database.hpp>
#include <scorum/chain/budget_objects.hpp>
#include <scorum/chain/dbs_account.hpp>
#include <scorum/chain/account_object.hpp>

#include <tuple>

namespace scorum {
namespace chain {

typedef std::vector<budget_with_schedule_id_type> _budget_with_schedule_ids_type;

dbs_budget::dbs_budget(database& db)
    : _base_type(db)
{
    db_impl().get_index<budget_index>().indicies().get<by_owner_name>();
}

dbs_budget::budget_ids_type dbs_budget::get_budgets(const account_name_type& owner) const
{
    budget_ids_type ret;

    auto it_pair = db_impl().get_index<budget_index>().indicies().get<by_owner_name>().equal_range(owner);
    auto it = it_pair.first;
    auto it_end = it_pair.second;
    FC_ASSERT(it == it_end, "budget not found");
    while (it != it_end)
    {
        ret.push_back(it->id);
        it++;
    }

    return ret;
}

const budget_object& dbs_budget::get_budget(budget_id_type id) const
{
    try
    {
        return db_impl().get<budget_object, by_id>(id);
    }
    FC_CAPTURE_AND_RETHROW() // to write __FILE__, __LINE__ from here
}

const budget_object& dbs_budget::get_any_budget(const account_name_type& owner) const
{
    auto it_pair = db_impl().get_index<budget_index>().indicies().get<by_owner_name>().equal_range(owner);
    auto it_begin = it_pair.first;
    auto it_end = it_pair.second;
    FC_ASSERT(it_begin == it_end, "budget not found");

    return *it_begin;
}

const budget_object& dbs_budget::create_genesis_budget(const asset& balance_in_scorum, const share_type& per_block)
{
    FC_ASSERT(balance_in_scorum.symbol == SCORUM_SYMBOL, "invalid asset type (symbol)");
    FC_ASSERT(per_block > 0, "invalid per_block");

    auto head_block_num = db_impl().head_block_num();

    const budget_object& new_budget = db_impl().create<budget_object>([&](budget_object& budget) {
        budget.created = SCORUM_GENESIS_TIME;
        budget.owner = SCORUM_ROOT_POST_PARENT;
        budget.balance = balance_in_scorum;
        budget.per_block = per_block;
        // allocate cash only after next block generation
        budget.block_last_allocated_for = head_block_num;
        budget.auto_close = false;
    });

    // there are not schedules for genesis budget
    return new_budget;
}

const budget_object& dbs_budget::create_budget(const account_object& owner,
                                               const optional<string>& content_permlink,
                                               const asset& balance_in_scorum,
                                               const share_type& per_block,
                                               bool auto_close)
{
    FC_ASSERT(owner.name == SCORUM_ROOT_POST_PARENT, "not allowed for ordinary budget");
    FC_ASSERT(balance_in_scorum.symbol == SCORUM_SYMBOL, "invalid asset type (symbol)");

    const dynamic_global_property_object& props = db_impl().get_dynamic_global_properties();

    dbs_account& account_service = db().obtain_service<dbs_account>();

    FC_ASSERT(owner.balance < balance_in_scorum, "insufficient funds");

    account_service.decrease_balance(owner, balance_in_scorum);

    auto head_block_num = db_impl().head_block_num();

    const budget_object& new_budget = db_impl().create<budget_object>([&](budget_object& budget) {
        budget.created = props.time;
        budget.owner = owner.name;
        budget.balance = balance_in_scorum;
        budget.per_block = per_block;
        // allocate cash only after next block generation
        budget.block_last_allocated_for = head_block_num;
        budget.auto_close = auto_close;
    });

    const budget_schedule_object& new_schedule = db_impl().create<budget_schedule_object>([&](budget_schedule_object&) {
        // Schedule with schedule_alg == unconditional is created.
        // Use adjust_budget_schedule to setup other algorithm
    });

    db_impl().create<budget_with_schedule_object>([&](budget_with_schedule_object& link) {
        link.budget = new_budget.id;
        link.schedule = new_schedule.id;
    });

    return new_budget;
}

void dbs_budget::close_budget(const budget_object& budget)
{
    FC_ASSERT(budget.owner == SCORUM_ROOT_POST_PARENT, "not allowed for genesis budget");

    dbs_account& account_service = db().obtain_service<dbs_account>();

    const auto& owner = account_service.get_account(budget.owner);

    // withdraw all balance rest asset back to owner
    //
    asset repayable = budget.balance;

    if (repayable.amount > 0)
    {
        decrease_balance(budget, repayable);
        account_service.increase_balance(owner, repayable);
    }

    // delete all budget schedules
    //

    _budget_with_schedule_ids_type schedule_links;
    auto it_pair
        = db_impl().get_index<budget_with_schedule_index>().indicies().get<by_budget_id>().equal_range(budget.id);
    auto it = it_pair.first;
    auto it_end = it_pair.second;
    FC_ASSERT(it != it_end, "link to schedule must exist for this type of budget");
    while (it != it_end)
    {
        schedule_links.push_back(it->id);
        it++;
    }
    for (const budget_with_schedule_id_type& link_id : schedule_links)
    {
        const budget_with_schedule_object& link = db_impl().get<budget_with_schedule_object, by_id>(link_id);
        auto schedule_id = link.schedule;
        db_impl().remove(db_impl().get<budget_schedule_object, by_id>(schedule_id));
        db_impl().remove(link);
    }

    // delete budget
    //
    db_impl().remove(budget);
}

void dbs_budget::increase_balance(const budget_object& budget, const asset& scorums)
{
    FC_ASSERT(scorums.symbol == SCORUM_SYMBOL, "invalid asset type (symbol)");

    db_impl().modify(budget, [&](budget_object& b) { b.balance += scorums; });
}

void dbs_budget::decrease_balance(const budget_object& budget, const asset& scorums)
{
    FC_ASSERT(scorums.symbol == SCORUM_SYMBOL, "invalid asset type (symbol)");

    if (budget.balance.amount > 0)
    {
        db_impl().modify(budget, [&](budget_object& b) { b.balance -= scorums; });
    }

    _check_autoclose(budget);
}

void dbs_budget::update_budget(const budget_object& budget, bool auto_close)
{
    FC_ASSERT(budget.owner == SCORUM_ROOT_POST_PARENT, "not allowed for genesis budget");

    db_impl().modify(budget, [&](budget_object& b) { b.auto_close = auto_close; });

    if (auto_close && budget.balance.amount <= 0)
    {
        close_budget(budget);
    }
}

dbs_budget::budget_schedule_ids_type dbs_budget::get_schedules(const budget_object& budget) const
{
    FC_ASSERT(budget.owner == SCORUM_ROOT_POST_PARENT, "not allowed for genesis budget");

    budget_schedule_ids_type ret;

    auto it_pair
        = db_impl().get_index<budget_with_schedule_index>().indicies().get<by_budget_id>().equal_range(budget.id);
    auto it = it_pair.first;
    auto it_end = it_pair.second;
    FC_ASSERT(it == it_end, "schedule not found");
    while (it != it_end)
    {
        ret.push_back(it->schedule);
        it++;
    }

    return ret;
}

const budget_schedule_object& dbs_budget::get_schedule(budget_schedule_id_type id) const
{
    try
    {
        return db_impl().get<budget_schedule_object, by_id>(id);
    }
    FC_CAPTURE_AND_RETHROW() // to write __FILE__, __LINE__ from here
}

const budget_schedule_object& dbs_budget::get_any_schedule(const budget_object& budget) const
{
    FC_ASSERT(budget.owner == SCORUM_ROOT_POST_PARENT, "not allowed for genesis budget");

    auto it_pair
        = db_impl().get_index<budget_with_schedule_index>().indicies().get<by_budget_id>().equal_range(budget.id);
    auto it_begin = it_pair.first;
    auto it_end = it_pair.second;
    FC_ASSERT(it_begin == it_end, "schedule not found");

    return get_schedule(it_begin->schedule);
}

void dbs_budget::append_schedule(const budget_object& budget,
                                 const optional<uint16_t>& schedule_alg,
                                 const optional<time_point_sec>& start_date,
                                 const optional<time_point_sec>& end_date,
                                 const optional<uint32_t>& period,
                                 const optional<time_point_sec>& now)
{
    FC_ASSERT(budget.owner == SCORUM_ROOT_POST_PARENT, "not allowed for genesis budget");

    uint16_t validated_schedule_alg = _validate_schedule_input(schedule_alg, start_date, end_date, period);

    const budget_schedule_object& new_schedule = db_impl().create<budget_schedule_object>([&](budget_schedule_object&) {
        // Schedule with schedule_alg == unconditional is created.
        // Use adjust_budget_schedule to setup other algorithm
    });

    db_impl().create<budget_with_schedule_object>([&](budget_with_schedule_object& link) {
        link.budget = budget.id;
        link.schedule = new_schedule.id;
    });

    _adjust_schedule(new_schedule, validated_schedule_alg, start_date, end_date, period, now);
}

void dbs_budget::adjust_schedule(const budget_schedule_object& budget_schedule,
                                 const optional<uint16_t>& schedule_alg,
                                 const optional<time_point_sec>& start_date,
                                 const optional<time_point_sec>& end_date,
                                 const optional<uint32_t>& period,
                                 const optional<time_point_sec>& now)
{
    // validate schedule input
    budget_schedule_object::budget_schedule_type en_schedule_alg = budget_schedule.schedule_alg;
    if (schedule_alg.valid())
    {
        en_schedule_alg = (budget_schedule_object::budget_schedule_type)(*schedule_alg);
    }
    uint16_t validated_schedule_alg = _validate_schedule_input((uint16_t)en_schedule_alg, start_date, end_date, period);

    _adjust_schedule(budget_schedule, validated_schedule_alg, start_date, end_date, period, now);
}

void dbs_budget::lock_schedule(const budget_schedule_object& schedule)
{
    db_impl().modify(schedule, [&](budget_schedule_object& s) { s.locked = true; });
}
void dbs_budget::unlock_schedule(const budget_schedule_object& schedule)
{
    db_impl().modify(schedule, [&](budget_schedule_object& s) { s.locked = false; });
}

void dbs_budget::remove_schedule(const budget_schedule_object& schedule)
{
    _budget_with_schedule_ids_type schedule_links;
    auto it_pair
        = db_impl().get_index<budget_with_schedule_index>().indicies().get<by_schedule_id>().equal_range(schedule.id);
    auto it = it_pair.first;
    auto it_end = it_pair.second;
    FC_ASSERT(it != it_end, "schedule link not found");
    while (it != it_end)
    {
        schedule_links.push_back(it->id);
        it++;
    }
    FC_ASSERT(1 == schedule_links.size(), "there is only one budget per schedule instance allowed for current version");

    auto link_id = (*schedule_links.begin());
    const budget_with_schedule_object& link = db_impl().get<budget_with_schedule_object, by_id>(link_id);

    const budget_object& budget = get_budget(link.budget);

    // check that at least one schedule must exists
    budget_schedule_ids_type schedules = get_schedules(budget);
    if (schedules.size() > 1)
    {
        db_impl().remove(schedule);
        db_impl().remove(link);
    }
    else
    {
        // if only one remains
        lock_schedule(schedule);
    }
}

void dbs_budget::crear_schedules(const budget_object& budget)
{
    FC_ASSERT(budget.owner == SCORUM_ROOT_POST_PARENT, "not allowed for genesis budget");

    budget_schedule_ids_type schedules = get_schedules(budget);
    for (auto schedule_id : schedules)
    {
        const budget_schedule_object& schedule = get_schedule(schedule_id);
        remove_schedule(schedule);
    }
}

asset dbs_budget::allocate_cash(const budget_object& budget, const optional<time_point_sec>& now)
{
    _time t = _get_now(now);

    asset ret(0, SCORUM_SYMBOL);

    auto head_block_num = db_impl().head_block_num();

    if (budget.block_last_allocated_for >= head_block_num)
    {
        return ret; // empty (allocation waits new block)
    }

    auto it_pair
        = db_impl().get_index<budget_with_schedule_index>().indicies().get<by_budget_id>().equal_range(budget.id);
    auto it = it_pair.first;
    auto it_end = it_pair.second;
    FC_ASSERT(it == it_end, "schedule not found");
    bool trapped = false;
    while (it != it_end)
    {
        const budget_schedule_object& schedule = get_schedule(it->schedule);
        if (schedule.locked)
        {
            continue;
        }

        switch (schedule.schedule_alg)
        {
        case budget_schedule_object::unconditional:
            trapped = true;
            break;
        case budget_schedule_object::by_period:
            if (t >= schedule.start_date && t <= schedule.start_date + schedule.period)
            {
                trapped = true;
            }
            break;
        case budget_schedule_object::by_time_range:
            if (t >= schedule.start_date && t <= schedule.end_date)
            {
                trapped = true;
            }
            break;
        default:;
        }

        if (trapped)
        {
            break;
        }
        it++;
    }

    if (trapped)
    {
        FC_ASSERT(budget.per_block > 0, "invalid per_block");
        ret = asset(budget.per_block, SCORUM_SYMBOL);
        decrease_balance(budget, ret);
        _check_autoclose(budget);
    }

    return ret;
}

void dbs_budget::_check_autoclose(const budget_object& budget)
{
    if (budget.auto_close)
    {
        const budget_object& modified_budget = get_budget(budget.id);
        if (modified_budget.balance.amount <= 0)
        {
            close_budget(modified_budget);
        }
    }
}

uint16_t dbs_budget::_validate_schedule_input(const optional<uint16_t>& schedule_alg,
                                              const optional<time_point_sec>& start_date,
                                              const optional<time_point_sec>& end_date,
                                              const optional<uint32_t>& period)
{
    budget_schedule_object::budget_schedule_type en_schedule_alg = budget_schedule_object::unconditional;
    if (schedule_alg.valid())
    {
        en_schedule_alg = (budget_schedule_object::budget_schedule_type)(*schedule_alg);
    }
    switch (en_schedule_alg)
    {
    case budget_schedule_object::unconditional:
        FC_ASSERT(!start_date.valid() && !end_date.valid() && !period.valid(), "invalid for unconditional algorithm");
        break;
    case budget_schedule_object::by_time_range:
    {
        FC_ASSERT((start_date.valid() || end_date.valid()) && !period.valid(), "invalid for date range algorithm");
        time_point_sec t_start = time_point_sec::min();
        if (start_date.valid())
            t_start = *start_date;
        time_point_sec t_end = time_point_sec::maximum();
        if (end_date.valid())
            t_end = *end_date;
        FC_ASSERT(t_start < t_end);
        break;
    }
    case budget_schedule_object::by_period:
        FC_ASSERT(period.valid(), "invalid for period algorithm");
        break;
    default:
        FC_ASSERT(false, "invalid algorithm");
    }
    return (uint16_t)en_schedule_alg;
}

void dbs_budget::_adjust_schedule(const budget_schedule_object& schedule,
                                  uint16_t schedule_alg,
                                  const optional<time_point_sec>& start_date,
                                  const optional<time_point_sec>& end_date,
                                  const optional<uint32_t>& period,
                                  const optional<time_point_sec>& now)
{
    _time t = _get_now(now);

    budget_schedule_object::budget_schedule_type en_schedule_alg
        = (budget_schedule_object::budget_schedule_type)schedule_alg;

    // setup schedule input
    db_impl().modify(schedule, [&](budget_schedule_object& s) {
        switch (en_schedule_alg)
        {
        case budget_schedule_object::unconditional:
        {
            s.start_date = time_point_sec::min();
            s.end_date = time_point_sec::maximum();
            s.period = time_point_sec::maximum().sec_since_epoch();
        }
        case budget_schedule_object::by_time_range:
        {
            if (start_date.valid())
            {
                s.start_date = *start_date;
            }
            else
            {
                s.start_date = t;
            }
            if (end_date.valid())
            {
                s.end_date = *end_date;
            }
            else
            {
                s.end_date = time_point_sec::maximum();
            }
            s.period = 0;
        }
        case budget_schedule_object::by_period:
        {
            s.start_date = t;
            s.end_date = time_point_sec::maximum();
            s.period = *period;
        }
        default:;
        }
    });
}
}
}