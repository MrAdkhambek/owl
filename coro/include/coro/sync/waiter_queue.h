#pragma once

// The two pieces every single-threaded resource that parks coroutines ends
// up writing: the queue they wait in, and the loop that resumes them.
//
// waiter_queue is an intrusive FIFO over nodes that live in the parked
// coroutines' own frames, so waiting allocates nothing. Link names the
// pointer that threads a particular queue, because a node can sit in two
// at once: a resource that hands the head to its successor while the head
// is still queued needs a second link, and sharing one link between the
// two queues silently severs the FIFO behind the node.
//
// resume_drain carries the subtle half. Resuming a waiter can lead
// straight back into the resource -- a resumed coroutine that releases
// what it was given, or finishes and hands on, before it suspends again --
// so a resumption raised while a drain is already running is queued and
// run by the outer call. The stack depth stays one however long the chain
// of waiters that complete without suspending.
//
// Neither is synchronised: both are for a resource used from one thread,
// which is what lets them be this cheap.

#include <concepts>
#include <coroutine>
#include <cstddef>

namespace coro {
    // A node this machinery can resume: it knows the coroutine waiting on
    // it, under the name both users already gave that member.
    template <typename Node>
    concept parked_node = requires(const Node& n) {
        { n.h } -> std::convertible_to<std::coroutine_handle<>>;
    };

    template <typename Node, Node* Node::* Link>
    class waiter_queue final {
    public:
        [[nodiscard]] bool empty() const noexcept {
            return head_ == nullptr;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

        // The node that will come out next, without taking it: what a
        // resource asks when it wants to resume the head in place.
        [[nodiscard]] Node* front() const noexcept {
            return head_;
        }

        void push(Node* const node) noexcept {
            node->*Link = nullptr;
            if (tail_ != nullptr) tail_->*Link = node;
            else head_ = node;
            tail_ = node;
            ++size_;
        }

        [[nodiscard]] Node* pop() noexcept {
            Node* const node = head_;
            if (node == nullptr) return nullptr;
            head_ = node->*Link;
            if (head_ == nullptr) tail_ = nullptr;
            node->*Link = nullptr;
            --size_;
            return node;
        }

    private:
        Node* head_{};
        Node* tail_{};
        std::size_t size_ = 0;
    };

    template <parked_node Node, Node* Node::* ReadyLink>
    class resume_drain final {
    public:
        void resume(Node* const node) noexcept {
            if (draining_) {
                ready_.push(node);
                return;
            }
            draining_ = true;
            node->h.resume();
            while (Node* const next = ready_.pop()) next->h.resume();
            draining_ = false;
        }

        // True while a resumption is in progress, for an assertion or for a
        // resource that wants to know it is being re-entered.
        [[nodiscard]] bool draining() const noexcept {
            return draining_;
        }

    private:
        waiter_queue<Node, ReadyLink> ready_;
        bool draining_ = false;
    };
}
