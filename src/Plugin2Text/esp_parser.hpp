#pragma once
#include "common.hpp"
#include "tes.hpp"
#include "parseutils.hpp"

struct RecordField {
    RecordFieldType type = (RecordFieldType)0;
    StaticArray<uint8_t> data;
};

struct RecordBase {
    RecordType type;
    RecordFlags flags;
    uint16_t timestamp = 0;
    uint8_t last_user_id = 0;
    uint8_t current_user_id = 0;
};

struct Record : RecordBase {
    FormID id;
    Array<RecordField*> fields;
    uint16_t version = 0;
    uint16_t unknown = 0;
};

struct GrupRecord : RecordBase {
    RecordGroupType group_type;
    Array<RecordBase*> records;
    union {
        struct {
            int16_t grid_y;
            int16_t grid_x;
        };
        uint32_t label;
    };
    uint32_t unknown;

    inline GrupRecord() : group_type(RecordGroupType::Top), label(0), unknown(0) { }
};

struct EspObjectModel {
    Array<RecordBase*> records;
};

struct EspParser {
    Slice buffer;

    EspObjectModel model;

    void init();
    void dispose();

    void parse(const StaticArray<uint8_t> data);
private:
    RecordField* process_field(Record* record, const RawRecordField* field);
    RecordBase* process_record(const RawRecord* record);
    void process_records(const uint8_t* start, const uint8_t* end);
};