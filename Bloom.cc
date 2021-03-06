#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "Bloom.h"
#include "murmurhash2.h"
#include "bloom.pb.h"

#define MAKESTRING(n) STRING(n)
#define STRING(n) #n

//==============================================================================
// Bloom

/**
 * // Create a bloom, which will `entries` elements, error ratio
 * // `err_mode/err_deno`, each bloomInstance with `slice_num` slices.
 * Bloom* bloom = new Bloom(4000, 1, 1000, 2);
 * bloom->Add("hello world");
 * bloom->Test("hello world");
 */
Bloom::Bloom(int entries, int err_mode, int err_deno, int slice_num, bool reset) {

    m_trans_period = TRANSITION_PERIOD_SECONDS;

    BloomInstance* instance = new BloomInstance(entries, err_mode, err_deno, slice_num, reset);
    m_instances.push_back(instance);
}

/**
 * Bloom() should be used combined with InitBloom(string& pb)
 */
Bloom::Bloom() {
}

Bloom::~Bloom() {
    for (auto it = m_instances.begin(); it != m_instances.end(); it++) {
        delete *it;
    }
}

/**
 * // Create and serialize the bloom
 * Bloom* bloom = new Bloom(4000, 1, 1000, 2);
 * bloom->Add("hello world");
 * string buf;
 * bloom->SaveBloom(&buf);
 *
 * // De-Serialize the bloom
 * Bloom* bloom = new Bloom();
 * bloom->InitBloom(buf);
 * bloom->Test("hello world");
 */
bool Bloom::InitBloom(string& pb) {

    pb_bloom::Bloom pbBloom;
    if( !pbBloom.ParseFromString(pb) ) {
        fprintf(stderr, "De-Serialize pb_bloom failed\n");
        return false;
    }

    // Bloom attr
    this->m_trans_period = pbBloom.trans_period();

    for (auto it = m_instances.begin(); it != m_instances.end(); it++) {
        delete *it;
    }
    this->m_instances.clear();

    // rebuild BloomInstance
    for (auto it = pbBloom.instances().begin(); it != pbBloom.instances().end(); it++) {


        auto instance = new BloomInstance;
        this->m_instances.push_back(instance);

        instance->SetEntries(it->entries());
        instance->SetErrMode(it->err_mode());
        instance->SetErrDeno(it->err_deno());
        instance->SetSliceNum(it->slices_size());
        instance->SetCreateTime(it->create_time());
        instance->SetReset(it->reset());

        // rebuild BloomSlice
        for (auto itSlice = it->slices().begin(); itSlice != it->slices().end(); itSlice++) {

            auto bloomSlice = new BloomSlice();
            instance->AddSlice(bloomSlice);

            bloomSlice->SetCreateTime(itSlice->create_time());
            bloomSlice->SetAccessTime(itSlice->access_time());
            bloomSlice->SetBits(itSlice->bits());
            bloomSlice->SetHashes(itSlice->hashes());

            vector<bitset<64>> data;
            for (auto itData = itSlice->data().begin(); itData != itSlice->data().end(); itData++) {
                data.push_back(bitset<64>(*itData));
            }
            bloomSlice->SetData(data);
        }
    }

    return true;
}

/**
 * // Serialize bloom
 * Bloom* bloom = new Bloom(4000, 1, 1000, 2);
 * bloom->Add("hello world");
 * string buf;
 * bloom->SaveBloom(buf);
 */
bool Bloom::SaveBloom(string &buf) {

    pb_bloom::Bloom pbBloom;

    // Bloom attr
    pbBloom.set_trans_period(m_trans_period);

    // BloomInstance
    for (auto it = m_instances.begin(); it != m_instances.end(); it++) {

        pb_bloom::BloomInstance* pbInstance = pbBloom.add_instances();

        // BloomInstance attr
        pbInstance->set_entries((*it)->GetEntries());
        pbInstance->set_err_mode((*it)->GetErrMode());
        pbInstance->set_err_deno((*it)->GetErrDeno());
        pbInstance->set_slice_num((*it)->GetSliceNum());
        pbInstance->set_create_time((*it)->GetCreateTime());
        pbInstance->set_reset((*it)->GetReset());

        // BloomSlice data
        vector<BloomSlice*> slices = (*it)->GetSlices();
        for (auto itSlice = slices.begin(); itSlice != slices.end(); itSlice++) {
            // BloomSlice attr
            pb_bloom::BloomSlice* pbSlice = pbInstance->add_slices();
            pbSlice->set_create_time((*itSlice)->GetCreateTime());
            pbSlice->set_access_time((*itSlice)->GetAccessTime());
            pbSlice->set_bits((*itSlice)->GetBits());
            pbSlice->set_hashes((*itSlice)->GetHashes());
            // BloomSlice data
            vector<bitset<64>> data = (*itSlice)->GetData();
            for (auto itData = data.begin(); itData != data.end(); itData++) {
                pbSlice->add_data(itData->to_ullong());
            }
        }
    }

    if( !pbBloom.SerializeToString(&buf) ) {
        fprintf(stderr, "pbBloom Serialize failed");
        return false;
    }

    return true;
}

bool Bloom::Add(string& key) {

    if (m_instances.size() == 0) {
        return false;
    }
    BloomInstance* instance = *(m_instances.rbegin());
    return instance->Add(key);
}

bool Bloom::Test(string& key) {

    for (auto it = m_instances.begin(); it != m_instances.end(); it++) {
        BloomInstance* instance = *it;
        if (instance->Test(key)) {
            return true;
        }
    }
    return false;
}

bool Bloom::Reset(RESET_TYPE type) {

    if (m_instances.size() == 0) {
        return false;
    }

    if (type == RESET_FIRST_INSTANCE_FIRST_SLICE) {
        BloomInstance* instance = *(m_instances.begin());

        if (instance->Reset()) {
            return true;
        } else {
            return false;
        }

    } else {
        fprintf(stderr, "not supported RESET_TYPE\n");
        return false;
    }
}

bool Bloom::NewBloomInstance(int entries, int err_mode, int err_deno, int slice_num, bool reset) {
    m_trans_period = TRANSITION_PERIOD_SECONDS;
    
    BloomInstance* instance = new BloomInstance(entries, err_mode, err_deno, slice_num, reset);
    if (!instance) {
        fprintf(stderr, "Cannot allocate memory for new BloomInstance\n");
        return false;
    }

    m_instances.push_back(instance);
    return true;
}

//==============================================================================
// BloomInstance

/**
 * Create a BloomInstance, this should only be called by Bloom logic.
 */
BloomInstance::BloomInstance(int entries, int err_mode, int err_deno, int slice_num, bool reset) 
    : m_entries(entries), m_err_mode(err_mode), m_err_deno(err_deno), m_slice_num(slice_num), m_reset(reset) {

    double avg_entries = entries / slice_num;
    double avg_error = ((double)err_mode / (double)err_deno) / m_slice_num;

    for (int i = 0; i < m_slice_num; i++) {
        BloomSlice* slice = new BloomSlice(avg_entries, avg_error);
        m_slices.push_back(slice);
    }

    m_create_time = time(NULL);
}

BloomInstance::BloomInstance() {
}

BloomInstance::~BloomInstance() {
    for (auto it = m_slices.begin(); it != m_slices.end(); it++) {
        delete *it;
    }
}

bool BloomInstance::Add(string& key) {

    for (auto it = m_slices.begin(); it != m_slices.end(); it++) {
        BloomSlice* slice = *it;
        if (!slice->Full()) {
            if(slice->Add(key)) {
                return true;
            } else {
                return false;
            }
        } 
    }

    return false;

    // If m_reset is true, automatically reset when detecting all slices are
    // full, so the error ratio will always be lower than we specified. The
    // cost is we lost one slice's history data.
    //
    // If you care about history data, you can create a Bloom with argument
    // Bloom(..., false) and call Bloom::Reset() manully to reset the slice
    // data.

    if (m_reset) {
        static uint32_t resetCnt = 0;
        resetCnt++;
    
        if (Reset()) {
            printf("BloomInstance Reset succ, happens times: %u\n", resetCnt);
            auto it = m_slices.rbegin();
            return (*it)->Add(key);
        } else {
            printf("BloomInstance Reset failed, happens times: %u\n", resetCnt);
            return false;
        }
    }
}

bool BloomInstance::Test(string& key) {
    for (auto it = m_slices.begin(); it != m_slices.end(); it++) {
        BloomSlice* slice = *it;

        if (slice->Test(key)) {
            return true;
        }
    }

    return false;
}

bool BloomInstance::Reset() {

    if (m_slice_num < 2) {
        fprintf(stderr, "invalid slice_num: %d\n", m_slice_num);
        return false;
    }

    // method 1
    /*
    auto ptr = *(m_slices.begin());
    m_slices.erase(m_slices.begin());
    delete ptr;

    double avg_entries = m_entries / m_slice_num;
    double avg_error = ((double)m_err_mode / (double)m_err_deno) / m_slice_num;

    BloomSlice* slice = new BloomSlice(avg_entries, avg_error);
    m_slices.push_back(slice);
    */

    // method 2
    auto ptr = *(m_slices.begin());
    m_slices.erase(m_slices.begin());
    m_slices.push_back(ptr);
    ptr->Reset();
    
    return true;
}

//==============================================================================
// BloomSlice

BloomSlice::BloomSlice(int entries, double error) {

    double num = log(error);
    double denom = 0.480453013918201; // ln(2)^2
    double bpe = -(num / denom);

    int bits = (int)(entries * bpe);
    int hashes = (int)ceil(0.693147180559945 * bpe); // ln(2)

    m_bits = bits;
    m_hashes = hashes;

    int elements;
    if (bits % 64) {
        elements = (bits / 64) + 1;
    } else {
        elements = bits / 64;
    }
    //printf("entries:%d, error:%lf, bits:%d, hashes:%d, elements:%d\n", entries, error, bits, hashes, elements);

    bitset<64> bit64;
    m_data.resize(elements, bit64);

    m_create_time = time(NULL);
}

BloomSlice::BloomSlice() {}

BloomSlice::~BloomSlice() {}

// if add success return true, or return false
bool BloomSlice::Add(string& key) {

    register unsigned int a = murmurhash2(key.c_str(), strlen(key.c_str()), 0x9747b28c);
    register unsigned int b = murmurhash2(key.c_str(), strlen(key.c_str()), a);

    register unsigned int x;
    register unsigned int i;

    int hits = 0;

    for (i = 0; i < (unsigned int)m_hashes; i++) {
        x = (a + i*b) % m_bits;
        if (testOk(x, true)) {
            hits++; 
        }
    }

    if (hits == m_hashes) {
        return false;
    }

    return true;
}

// if bit at pos is set return true, or return false
bool BloomSlice::testOk(int pos, bool needSet) {

    unsigned int bitset_idx = pos >> 6;

    if (m_data.size() <= bitset_idx) {
        fprintf(stderr, "data maybe corrupted\n");
        return false;
    }

    if (m_data[bitset_idx].test(pos % 64)) {
        return true;
    } else {
        if (needSet) {
            m_data[bitset_idx].set(pos % 64);
        }
        return false;
    }
}

// if key is already stored, returns true
bool BloomSlice::Test(string& key) {
    
    register unsigned int a = murmurhash2(key.c_str(), strlen(key.c_str()), 0x9747b28c);
    register unsigned int b = murmurhash2(key.c_str(), strlen(key.c_str()), a);

    int hits = 0;

    register unsigned int x;
    register unsigned int i;

    for (i = 0; i < (unsigned int)m_hashes; i++) {
        x = (a + i*b) % m_bits;
        //printf("hash:%d, bit pos: %d\n", i, x);
        if (testOk(x)) {
            hits++; 
        }
    }

    if (hits == m_hashes) {
        return true;
    }

    return false;
}

// if bits of ones takes up equal or larger than one half of m_bits, returns full
bool BloomSlice::Full() {

    int ones = 0;
    for (auto it = m_data.begin(); it != m_data.end(); it++) {
        ones += it->count();
    }

    //printf("Slice ones: %d, m_bits: %d\n", ones, m_bits);

    if (ones >= m_bits / 2) {
        return true;
    }

    return false;
}

void BloomSlice::Reset() {
    for (auto it = m_data.begin(); it != m_data.end(); it++) {
        *it = 0;
    }
}
