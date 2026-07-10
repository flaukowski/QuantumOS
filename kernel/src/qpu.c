/**
 * QuantumOS QPU job broker implementation (epic #148 A2+A3).
 * See kernel/include/kernel/qpu.h for the model and lifecycle; every entry
 * point runs cli'd in syscall context on one CPU (no locking), and the
 * pointers arrive pre-validated by the syscall layer.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/qpu.h>
#include <kernel/audit.h>
#include <kernel/capability.h>
#include <kernel/manifest.h>
#include <kernel/process.h>
#include <kernel/syscall.h> /* SYSCALL_* error codes */

typedef struct {
    uint8_t state;    /* QPU_JOB_* */
    uint8_t orphaned; /* owner died while RUNNING; COMPLETE scrubs instead of DONE */
    uint8_t status;   /* QPU_STATUS_* (valid when DONE) */
    uint8_t reserved;
    uint32_t job_id; /* monotonic, 0 = never assigned */
    uint32_t owner_pid;
    uint32_t owner_generation;
    uint32_t executor_pid; /* stamped at FETCH — forgery defense + reclamation */
    uint32_t executor_generation;
    uint32_t circuit_len;
    uint32_t result_len;
    uint8_t circuit[QPU_CIRCUIT_MAX];
    uint8_t result[QPU_RESULT_MAX];
} qpu_job_t;

static qpu_job_t jobs[QPU_MAX_JOBS];
static uint32_t next_job_id = 1;

/* Scrub + free: a recycled slot must never leak a previous owner's circuit
 * or result bytes to the next occupant (pid-reuse info-leak guard). */
static void slot_free(qpu_job_t *j) {
    for (uint32_t i = 0; i < QPU_CIRCUIT_MAX; i++) {
        j->circuit[i] = 0;
    }
    for (uint32_t i = 0; i < QPU_RESULT_MAX; i++) {
        j->result[i] = 0;
    }
    j->state = QPU_JOB_FREE;
    j->orphaned = 0;
    j->status = 0;
    j->job_id = 0;
    j->owner_pid = 0;
    j->owner_generation = 0;
    j->executor_pid = 0;
    j->executor_generation = 0;
    j->circuit_len = 0;
    j->result_len = 0;
}

uint64_t qpu_submit(uint32_t pid, const qpu_submit_req_t *req) {
    /* Quota precheck BEFORE any side effect (the sys_spawn contract): a
     * refusal records AUDIT_QUOTA and consumes nothing. */
    if (!manifest_qsub_precheck(pid)) {
        return SYSCALL_EPERM;
    }
    if (req->circuit_len == 0 || req->circuit_len > QPU_CIRCUIT_MAX) {
        return SYSCALL_EINVAL;
    }

    /* Per-owner in-flight cap: the qsub quota is a LIFETIME counter, not a
     * concurrency bound — without this, one submitter could pin the whole
     * 4-slot pool by never polling, starving every other holder (EAGAIN
     * before the charge, so a refused submit consumes no quota). */
    uint32_t inflight = 0;
    for (uint32_t i = 0; i < QPU_MAX_JOBS; i++) {
        if (jobs[i].state != QPU_JOB_FREE && jobs[i].owner_pid == pid) {
            inflight++;
        }
    }
    if (inflight >= QPU_INFLIGHT_MAX) {
        return SYSCALL_EIO; /* queue full for this owner — retry later */
    }

    qpu_job_t *slot = NULL;
    for (uint32_t i = 0; i < QPU_MAX_JOBS; i++) {
        if (jobs[i].state == QPU_JOB_FREE) {
            slot = &jobs[i];
            break;
        }
    }
    if (!slot) {
        return SYSCALL_EIO; /* pool exhausted — retry later */
    }

    for (uint32_t i = 0; i < req->circuit_len; i++) {
        slot->circuit[i] = req->circuit[i];
    }
    slot->circuit_len = req->circuit_len;
    slot->owner_pid = pid;
    slot->owner_generation = process_get_generation(pid);
    slot->job_id = next_job_id++;
    if (next_job_id == 0) {
        next_job_id = 1; /* 0 means never-assigned */
    }
    slot->state = QPU_JOB_PENDING;

    /* Charge LAST (accept is the terminal step) and record the exercised
     * WRITE authority — quota-bounded and field-identical across submits,
     * so a burst coalesces instead of evicting ledger evidence. */
    manifest_qsub_charge(pid);
    audit_record(AUDIT_QSUBMIT, pid, AUDIT_V_OK, CAP_RESOURCE_DEVICE, DEVICE_ID_QPU, CAP_WRITE);
    return slot->job_id;
}

uint64_t qpu_fetch(uint32_t pid, qpu_fetch_out_t *out) {
    /* Oldest PENDING = smallest job_id (monotonic). */
    qpu_job_t *oldest = NULL;
    for (uint32_t i = 0; i < QPU_MAX_JOBS; i++) {
        if (jobs[i].state == QPU_JOB_PENDING && (!oldest || jobs[i].job_id < oldest->job_id)) {
            oldest = &jobs[i];
        }
    }
    if (!oldest) {
        return 0; /* nothing pending */
    }
    oldest->state = QPU_JOB_RUNNING;
    oldest->executor_pid = pid;
    oldest->executor_generation = process_get_generation(pid);
    out->job_id = oldest->job_id;
    out->owner_pid = oldest->owner_pid;
    out->circuit_len = oldest->circuit_len;
    for (uint32_t i = 0; i < oldest->circuit_len; i++) {
        out->circuit[i] = oldest->circuit[i];
    }
    return oldest->job_id;
}

uint64_t qpu_complete(uint32_t pid, const qpu_complete_req_t *req) {
    if (req->result_len > QPU_RESULT_MAX) {
        return SYSCALL_EINVAL;
    }
    for (uint32_t i = 0; i < QPU_MAX_JOBS; i++) {
        qpu_job_t *j = &jobs[i];
        if (j->state != QPU_JOB_RUNNING || j->job_id != req->job_id) {
            continue;
        }
        /* The completer must BE the executor that fetched this job (same
         * pid, same incarnation): a second EXECUTE holder or a stale
         * watchdog-reborn qpud must not forge a result into another
         * owner's slot. Provable, not silent. */
        if (j->executor_pid != pid || j->executor_generation != process_get_generation(pid)) {
            audit_deny(pid, CAP_RESOURCE_DEVICE, DEVICE_ID_QPU, CAP_EXECUTE);
            return SYSCALL_EPERM;
        }
        if (j->orphaned) {
            /* Owner died mid-run: drop the result, free the slot. */
            slot_free(j);
            return 0;
        }
        j->status = (req->status == QPU_STATUS_OK) ? QPU_STATUS_OK : QPU_STATUS_EXECFAIL;
        j->result_len = req->result_len;
        for (uint32_t k = 0; k < req->result_len; k++) {
            j->result[k] = req->result[k];
        }
        j->state = QPU_JOB_DONE;
        return 0;
    }
    return SYSCALL_ENOENT;
}

uint64_t qpu_poll(uint32_t pid, uint32_t job_id, qpu_poll_out_t *out) {
    for (uint32_t i = 0; i < QPU_MAX_JOBS; i++) {
        qpu_job_t *j = &jobs[i];
        if (j->state == QPU_JOB_FREE || j->job_id != job_id) {
            continue;
        }
        /* Full-triple ownership match: job_id + pid + live generation. A
         * recycled pid can never read a predecessor's result (the destroy
         * sweep frees those anyway — belt and braces). */
        if (j->owner_pid != pid || j->owner_generation != process_get_generation(pid)) {
            return SYSCALL_ENOENT;
        }
        if (j->state == QPU_JOB_PENDING) {
            out->state = QPU_POLL_PENDING;
            out->status = 0;
            out->result_len = 0;
            return QPU_POLL_PENDING;
        }
        if (j->state == QPU_JOB_RUNNING) {
            out->state = QPU_POLL_RUNNING;
            out->status = 0;
            out->result_len = 0;
            return QPU_POLL_RUNNING;
        }
        /* DONE: copy the result out, free the slot (single-consumer). */
        out->state = QPU_POLL_DONE;
        out->status = j->status;
        out->result_len = j->result_len;
        for (uint32_t k = 0; k < j->result_len; k++) {
            out->result[k] = j->result[k];
        }
        slot_free(j);
        return QPU_POLL_DONE;
    }
    return SYSCALL_ENOENT;
}

void qpu_on_process_destroy(uint32_t pid) {
    /* The death sweep — job slots are a per-process kernel resource, and a
     * dead process can never release its own (the fd/ipc/cap/manifest
     * precedent in process_destroy). Every pid-reuse path funnels through
     * process_destroy, so slots keyed on this pid belong to the dying
     * incarnation by construction. */
    for (uint32_t i = 0; i < QPU_MAX_JOBS; i++) {
        qpu_job_t *j = &jobs[i];
        if (j->state == QPU_JOB_FREE) {
            continue;
        }
        if (j->owner_pid == pid) {
            if (j->state == QPU_JOB_RUNNING) {
                /* The executor may be mid-execution on this circuit — let
                 * its COMPLETE scrub the slot rather than freeing a buffer
                 * it is about to write. */
                j->orphaned = 1;
            } else {
                slot_free(j); /* PENDING or DONE: nothing references it */
            }
        } else if (j->executor_pid == pid && j->state == QPU_JOB_RUNNING) {
            if (j->orphaned) {
                slot_free(j); /* owner already dead too — nobody left to poll */
            } else {
                /* Executor died mid-run: fail CLOSED so the owner's POLL
                 * observes EXECFAIL and frees the slot — never a silent
                 * RUNNING-forever orphan (4 of those = permanent EAGAIN). */
                j->state = QPU_JOB_DONE;
                j->status = QPU_STATUS_EXECFAIL;
                j->result_len = 0;
            }
        }
    }
}
