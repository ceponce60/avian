/* Copyright (c) 2008-2011, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "bootimage.h"
#include "heap.h"
#include "heapwalk.h"
#include "common.h"
#include "machine.h"
#include "util.h"
#include "stream.h"
#include "assembler.h"
#include "target.h"

// since we aren't linking against libstdc++, we must implement this
// ourselves:
extern "C" void __cxa_pure_virtual(void) { abort(); }

using namespace vm;

namespace {

const unsigned HeapCapacity = 256 * 1024 * 1024;

const unsigned TargetFixieSizeInBytes = 8 + (TargetBytesPerWord * 2);
const unsigned TargetFixieSizeInWords = ceiling
  (TargetFixieSizeInBytes, TargetBytesPerWord);
const unsigned TargetFixieAge = 0;
const unsigned TargetFixieHasMask = 1;
const unsigned TargetFixieSize = 4;

const bool DebugNativeTarget = false;

enum Type {
  Type_none,
  Type_object,
  Type_int8_t,
  Type_uint8_t,
  Type_int16_t,
  Type_uint16_t,
  Type_int32_t,
  Type_uint32_t,
  Type_intptr_t,
  Type_uintptr_t,
  Type_int64_t,
  Type_int64_t_pad,
  Type_uint64_t,
  Type_float,
  Type_double,
  Type_double_pad,
  Type_word,
  Type_array
};

class Field {
 public:
  Field(Type type, unsigned offset, unsigned targetOffset):
    type(type), offset(offset), targetOffset(targetOffset)
  { }

  Type type;
  unsigned offset;
  unsigned targetOffset;
};

class TypeMap {
 public:
  enum Kind {
    NormalKind,
    SingletonKind,
    PoolKind
  };

  TypeMap(unsigned buildFixedSizeInWords, unsigned targetFixedSizeInWords,
          unsigned fixedFieldCount, Kind kind = NormalKind,
          unsigned buildArrayElementSizeInBytes = 0,
          unsigned targetArrayElementSizeInBytes = 0,
          Type arrayElementType = Type_none):
    buildFixedSizeInWords(buildFixedSizeInWords),
    targetFixedSizeInWords(targetFixedSizeInWords),
    fixedFieldCount(fixedFieldCount),
    buildArrayElementSizeInBytes(buildArrayElementSizeInBytes),
    targetArrayElementSizeInBytes(targetArrayElementSizeInBytes),
    arrayElementType(arrayElementType),
    kind(kind)
  { }

  uintptr_t* targetFixedOffsets() {
    return reinterpret_cast<uintptr_t*>(this + 1);
  }

  Field* fixedFields() {
    return reinterpret_cast<Field*>
      (targetFixedOffsets() + (buildFixedSizeInWords * BytesPerWord));
  }

  static unsigned sizeInBytes(unsigned buildFixedSizeInWords,
                              unsigned fixedFieldCount)
  {
    return sizeof(TypeMap)
      + (buildFixedSizeInWords * BytesPerWord * BytesPerWord)
      + (sizeof(Field) * fixedFieldCount);
  }

  unsigned buildFixedSizeInWords;
  unsigned targetFixedSizeInWords;
  unsigned fixedFieldCount;
  unsigned buildArrayElementSizeInBytes;
  unsigned targetArrayElementSizeInBytes;
  Type arrayElementType;
  Kind kind;
};

// Notes on immutable references in the heap image:
//
// One of the advantages of a bootimage-based build is that reduces
// the overhead of major GCs at runtime since we can avoid scanning
// the pre-built heap image entirely.  However, this only works if we
// can ensure that no part of the heap image (with exceptions noted
// below) ever points to runtime-allocated objects.  Therefore (most)
// references in the heap image are considered immutable, and any
// attempt to update them at runtime will cause the process to abort.
//
// However, some references in the heap image really must be updated
// at runtime: e.g. the static field table for each class.  Therefore,
// we allocate these as "fixed" objects, subject to mark-and-sweep
// collection, instead of as "copyable" objects subject to copying
// collection.  This strategy avoids the necessity of maintaining
// "dirty reference" bitsets at runtime for the entire heap image;
// each fixed object has its own bitset specific to that object.
//
// In addition to the "fixed" object solution, there are other
// strategies available to avoid attempts to update immutable
// references at runtime:
//
//  * Table-based: use a lazily-updated array or vector to associate
//    runtime data with heap image objects (see
//    e.g. getClassRuntimeData in machine.cpp).
//
//  * Update references at build time: for example, we set the names
//    of primitive classes before generating the heap image so that we
//    need not populate them lazily at runtime.

bool
endsWith(const char* suffix, const char* s, unsigned length)
{
  unsigned suffixLength = strlen(suffix);
  return length >= suffixLength
    and memcmp(suffix, s + (length - suffixLength), suffixLength) == 0;
}

object
makeCodeImage(Thread* t, Zone* zone, BootImage* image, uint8_t* code,
              uintptr_t* codeMap, const char* className,
              const char* methodName, const char* methodSpec, object typeMaps)
{
  PROTECT(t, typeMaps);

  object constants = 0;
  PROTECT(t, constants);
  
  object calls = 0;
  PROTECT(t, calls);

  DelayedPromise* addresses = 0;

  Finder* finder = static_cast<Finder*>
    (systemClassLoaderFinder(t, root(t, Machine::BootLoader)));

  for (Finder::Iterator it(finder); it.hasMore();) {
    unsigned nameSize = 0;
    const char* name = it.next(&nameSize);

    if (endsWith(".class", name, nameSize)
        and (className == 0 or strncmp(name, className, nameSize - 6) == 0))
    {
      // fprintf(stderr, "%.*s\n", nameSize - 6, name);
      object c = resolveSystemClass
        (t, root(t, Machine::BootLoader),
         makeByteArray(t, "%.*s", nameSize - 6, name), true);

      PROTECT(t, c);

      System::Region* region = finder->find(name);
      
      { THREAD_RESOURCE(t, System::Region*, region, region->dispose());

        class Client: public Stream::Client {
         public:
          Client(Thread* t): t(t) { }

          virtual void NO_RETURN handleError() {
            vm::abort(t);
          }

         private:
          Thread* t;
        } client(t);

        Stream s(&client, region->start(), region->length());

        uint32_t magic = s.read4();
        expect(t, magic == 0xCAFEBABE);
        s.read2(); // minor version
        s.read2(); // major version

        unsigned count = s.read2() - 1;
        if (count) {
          Type types[count + 2];
          types[0] = Type_object;
          types[1] = Type_intptr_t;

          for (unsigned i = 2; i < count + 2; ++i) {
            switch (s.read1()) {
            case CONSTANT_Class:
            case CONSTANT_String:
              types[i] = Type_object;
              s.skip(2);
              break;

            case CONSTANT_Integer:
            case CONSTANT_Float:
              types[i] = Type_int32_t;
              s.skip(4);
              break;

            case CONSTANT_NameAndType:
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref:
              types[i] = Type_object;
              s.skip(4);
              break;

            case CONSTANT_Long:
              types[i++] = Type_int64_t;
              types[i] = Type_int64_t_pad;
              s.skip(8);
              break;

            case CONSTANT_Double:
              types[i++] = Type_double;
              types[i] = Type_double_pad;
              s.skip(8);
              break;

            case CONSTANT_Utf8:
              types[i] = Type_object;
              s.skip(s.read2());
              break;

            default: abort(t);
            }
          }

          object array = makeByteArray
            (t, TypeMap::sizeInBytes(count + 2, count + 2));

          TypeMap* map = new (&byteArrayBody(t, array, 0)) TypeMap
            (count + 2, count + 2, count + 2, TypeMap::PoolKind);

          for (unsigned i = 0; i < count + 2; ++i) {
            expect(t, i < map->buildFixedSizeInWords);

            map->targetFixedOffsets()[i * BytesPerWord]
              = i * TargetBytesPerWord;

            new (map->fixedFields() + i) Field
              (types[i], i * BytesPerWord, i * TargetBytesPerWord);
          }

          hashMapInsert
            (t, typeMaps, hashMapFind
             (t, root(t, Machine::PoolMap), c, objectHash, objectEqual), array,
             objectHash);
        }
      }

      if (classFieldTable(t, c)) {
        unsigned count = arrayLength(t, classFieldTable(t, c));

        Type memberTypes[count + 1];
        memberTypes[0] = Type_object;
        unsigned buildMemberOffsets[count + 1];
        buildMemberOffsets[0] = 0;
        unsigned targetMemberOffsets[count + 1];
        targetMemberOffsets[0] = 0;
        unsigned memberIndex = 1;
        unsigned buildMemberOffset = BytesPerWord;
        unsigned targetMemberOffset = TargetBytesPerWord;

        Type staticTypes[count + 2];
        staticTypes[0] = Type_object;
        staticTypes[1] = Type_intptr_t;
        unsigned buildStaticOffsets[count + 2];
        buildStaticOffsets[0] = 0;
        buildStaticOffsets[1] = BytesPerWord;
        unsigned targetStaticOffsets[count + 2];
        targetStaticOffsets[0] = 0;
        targetStaticOffsets[1] = TargetBytesPerWord;
        unsigned staticIndex = 2;
        unsigned buildStaticOffset = BytesPerWord * 2;
        unsigned targetStaticOffset = TargetBytesPerWord * 2;

        for (unsigned i = 0; i < count; ++i) {
          object field = arrayBody(t, classFieldTable(t, c), i);
          unsigned size = fieldSize(t, fieldCode(t, field));

          Type type;
          switch (fieldCode(t, field)) {
          case ObjectField:
            type = Type_object;
            size = TargetBytesPerWord;
            break;

          case ByteField:
          case BooleanField:
            type = Type_int8_t;
            break;

          case CharField:
          case ShortField:
            type = Type_int8_t;
            break;

          case FloatField:
          case IntField:
            type = Type_int32_t;
            break;

          case LongField:
          case DoubleField:
            type = Type_int64_t;
            break;

          default: abort(t);
          }

          if (fieldFlags(t, field) & ACC_STATIC) {
            staticTypes[staticIndex] = type;

            while (targetStaticOffset % size) {
              ++ targetStaticOffset;
            }

            targetStaticOffsets[staticIndex] = targetStaticOffset;

            targetStaticOffset += size;

            buildStaticOffset = fieldOffset(t, field);
            buildStaticOffsets[staticIndex] = buildStaticOffset;

            ++ staticIndex;
          } else {
            memberTypes[memberIndex] = type;

            while (targetMemberOffset % size) {
              ++ targetMemberOffset;
            }

            targetMemberOffsets[memberIndex] = targetMemberOffset;

            targetMemberOffset += size;

            buildMemberOffset = fieldOffset(t, field);
            buildMemberOffsets[memberIndex] = buildMemberOffset;

            ++ memberIndex;
          }
        }

        { object array = makeByteArray
            (t, TypeMap::sizeInBytes
             (ceiling(classFixedSize(t, c), BytesPerWord), memberIndex));

          TypeMap* map = new (&byteArrayBody(t, array, 0)) TypeMap
            (ceiling(classFixedSize(t, c), BytesPerWord),
             ceiling(targetMemberOffset, TargetBytesPerWord), memberIndex);

          for (unsigned i = 0; i < memberIndex; ++i) {
            expect(t, buildMemberOffsets[i]
                   < map->buildFixedSizeInWords * BytesPerWord);

            map->targetFixedOffsets()[buildMemberOffsets[i]]
              = targetMemberOffsets[i];

            new (map->fixedFields() + i) Field
              (memberTypes[i], buildMemberOffsets[i], targetMemberOffsets[i]);
          }

          hashMapInsert(t, typeMaps, c, array, objectHash);
        }

        if (classStaticTable(t, c)) {
          object array = makeByteArray
            (t, TypeMap::sizeInBytes
             (singletonCount(t, classStaticTable(t, c)) + 2, staticIndex));

          TypeMap* map = new (&byteArrayBody(t, array, 0)) TypeMap
            (singletonCount(t, classStaticTable(t, c)) + 2,
             ceiling(targetStaticOffset, TargetBytesPerWord), staticIndex,
             TypeMap::SingletonKind);

          for (unsigned i = 0; i < staticIndex; ++i) {
            expect(t, buildStaticOffsets[i]
                   < map->buildFixedSizeInWords * BytesPerWord);

            map->targetFixedOffsets()[buildStaticOffsets[i]]
              = targetStaticOffsets[i];

            new (map->fixedFields() + i) Field
              (staticTypes[i], buildStaticOffsets[i], targetStaticOffsets[i]);
          }

          hashMapInsert
            (t, typeMaps, classStaticTable(t, c), array, objectHash);
        }
      }

      if (classMethodTable(t, c)) {
        for (unsigned i = 0; i < arrayLength(t, classMethodTable(t, c)); ++i) {
          object method = arrayBody(t, classMethodTable(t, c), i);
          if (((methodName == 0
                or ::strcmp
                (reinterpret_cast<char*>
                 (&byteArrayBody
                  (t, vm::methodName(t, method), 0)), methodName) == 0)
               and (methodSpec == 0
                    or ::strcmp
                    (reinterpret_cast<char*>
                     (&byteArrayBody
                      (t, vm::methodSpec(t, method), 0)), methodSpec)
                    == 0)))
          {
            if (methodCode(t, method)
                or (methodFlags(t, method) & ACC_NATIVE))
            {
              PROTECT(t, method);

              t->m->processor->compileMethod
                (t, zone, &constants, &calls, &addresses, method);
            }

            object addendum = methodAddendum(t, method);
            if (addendum and methodAddendumExceptionTable(t, addendum)) {
              PROTECT(t, addendum);

              // resolve exception types now to avoid trying to update
              // immutable references at runtime
              for (unsigned i = 0; i < shortArrayLength
                     (t, methodAddendumExceptionTable(t, addendum)); ++i)
              {
                uint16_t index = shortArrayBody
                  (t, methodAddendumExceptionTable(t, addendum), i) - 1;

                object o = singletonObject
                  (t, addendumPool(t, addendum), index);

                if (objectClass(t, o) == type(t, Machine::ReferenceType)) {
                  o = resolveClass
                    (t, root(t, Machine::BootLoader), referenceName(t, o));
    
                  set(t, addendumPool(t, addendum),
                      SingletonBody + (index * BytesPerWord), o);
                }
              }
            }
          }
        }
      }
    }
  }

  for (; calls; calls = tripleThird(t, calls)) {
    object method = tripleFirst(t, calls);
    uintptr_t address;
    if (methodFlags(t, method) & ACC_NATIVE) {
      address = reinterpret_cast<uintptr_t>(code + image->thunks.native.start);
    } else {
      address = codeCompiled(t, methodCode(t, method));
    }

    static_cast<ListenPromise*>(pointerValue(t, tripleSecond(t, calls)))
      ->listener->resolve(address, 0);
  }

  for (; addresses; addresses = addresses->next) {
    uint8_t* value = reinterpret_cast<uint8_t*>(addresses->basis->value());
    expect(t, value >= code);

    void* location;
    bool flat = addresses->listener->resolve
      (reinterpret_cast<int64_t>(code), &location);
    target_uintptr_t offset = value - code;
    if (flat) {
      offset |= BootFlatConstant;
    }
    memcpy(location, &offset, TargetBytesPerWord);

    expect(t, reinterpret_cast<intptr_t>(location)
           >= reinterpret_cast<intptr_t>(code));

    markBit(codeMap, reinterpret_cast<intptr_t>(location)
            - reinterpret_cast<intptr_t>(code));
  }

  return constants;
}

unsigned
objectSize(Thread* t, object o)
{
  return baseSize(t, o, objectClass(t, o));
}

void
visitRoots(Thread* t, BootImage* image, HeapWalker* w, object constants)
{
  Machine* m = t->m;

  for (HashMapIterator it(t, classLoaderMap(t, root(t, Machine::BootLoader)));
       it.hasMore();)
  {
    w->visitRoot(tripleSecond(t, it.next()));
  }

  image->bootLoader = w->visitRoot(root(t, Machine::BootLoader));
  image->appLoader = w->visitRoot(root(t, Machine::AppLoader));
  image->types = w->visitRoot(m->types);

  m->processor->visitRoots(t, w);

  for (; constants; constants = tripleThird(t, constants)) {
    w->visitRoot(tripleFirst(t, constants));
  }
}

TypeMap*
typeMap(Thread* t, object typeMaps, object p)
{
  return reinterpret_cast<TypeMap*>
    (&byteArrayBody
     (t, objectClass(t, p) == type(t, Machine::SingletonType)
      ? hashMapFind(t, typeMaps, p, objectHash, objectEqual)
      : hashMapFind(t, typeMaps, objectClass(t, p), objectHash, objectEqual),
      0));
}

unsigned
targetOffset(Thread* t, object typeMaps, object p, unsigned offset)
{
  TypeMap* map = typeMap(t, typeMaps, p);

  if (map->targetArrayElementSizeInBytes
      and offset >= map->buildFixedSizeInWords * BytesPerWord)
  {
    return (map->targetFixedSizeInWords * TargetBytesPerWord)
      + (((offset - (map->buildFixedSizeInWords * BytesPerWord))
          / map->buildArrayElementSizeInBytes)
         * map->targetArrayElementSizeInBytes);
  } else {
    return map->targetFixedOffsets()[offset];
  }
}

unsigned
targetSize(Thread* t, object typeMaps, object p)
{
  TypeMap* map = typeMap(t, typeMaps, p);

  if (map->targetArrayElementSizeInBytes) {
    return map->targetFixedSizeInWords
      + ceiling(map->targetArrayElementSizeInBytes
                * cast<uintptr_t>
                (p, (map->buildFixedSizeInWords - 1) * BytesPerWord),
                TargetBytesPerWord);
  } else {
    switch (map->kind) {
    case TypeMap::NormalKind:
      return map->targetFixedSizeInWords;

    case TypeMap::SingletonKind:
      return map->targetFixedSizeInWords + singletonMaskSize
        (map->targetFixedSizeInWords - 2, TargetBitsPerWord);

    case TypeMap::PoolKind: {
      unsigned maskSize = poolMaskSize
        (map->targetFixedSizeInWords - 2, TargetBitsPerWord);

      return map->targetFixedSizeInWords + maskSize + singletonMaskSize
        (map->targetFixedSizeInWords - 2 + maskSize, TargetBitsPerWord);
    }

    default: abort(t);
    }
  }
}

void
copy(Thread* t, uint8_t* src, uint8_t* dst, Type type)
{
  switch (type) {
  case Type_int8_t:
    memcpy(dst, src, 1);
    break;

  case Type_int16_t: {
    int16_t s; memcpy(&s, src, 2);
    int16_t d = TARGET_V2(s);
    memcpy(dst, &d, 2);
  } break;

  case Type_int32_t:
  case Type_float: {
    int32_t s; memcpy(&s, src, 4);
    int32_t d = TARGET_V4(s);
    memcpy(dst, &d, 4);
  } break;

  case Type_int64_t:
  case Type_double: {
    int64_t s; memcpy(&s, src, 8);
    int64_t d = TARGET_V8(s);
    memcpy(dst, &d, 8);
  } break;

  case Type_int64_t_pad:
  case Type_double_pad:
    break;

  case Type_intptr_t: {
    intptr_t s; memcpy(&s, src, BytesPerWord);
    target_intptr_t d = TARGET_VW(s);
    memcpy(dst, &d, TargetBytesPerWord);
  } break;

  case Type_object: {
    memset(dst, 0, TargetBytesPerWord);
  } break;

  default: abort(t);
  }
}

bool
nonObjectsEqual(uint8_t* src, uint8_t* dst, Type type)
{
  switch (type) {
  case Type_int8_t:
    return memcmp(dst, src, 1) == 0;

  case Type_int16_t:
    return memcmp(dst, src, 2) == 0;

  case Type_int32_t:
  case Type_float:
    return memcmp(dst, src, 4) == 0;

  case Type_int64_t:
  case Type_double:
    return memcmp(dst, src, 8) == 0;

  case Type_int64_t_pad:
  case Type_double_pad:
    return true;

  case Type_intptr_t:
    return memcmp(dst, src, BytesPerWord) == 0;

  case Type_object:
    return true;

  default: abort();
  }  
}

bool
nonObjectsEqual(TypeMap* map, uint8_t* src, uint8_t* dst)
{
  for (unsigned i = 0; i < map->fixedFieldCount; ++i) {
    Field* field = map->fixedFields() + i;
    if (not nonObjectsEqual
        (src + field->offset, dst + field->targetOffset, field->type))
    {
      return false;
    }
  }

  if (map->targetArrayElementSizeInBytes) {
    unsigned fixedSize = map->buildFixedSizeInWords * BytesPerWord;
    unsigned count = cast<uintptr_t>(src, fixedSize - BytesPerWord);

    for (unsigned i = 0; i < count; ++i) {
      if (not nonObjectsEqual
          (src + fixedSize + (i * map->buildArrayElementSizeInBytes),
           dst + (map->targetFixedSizeInWords * TargetBytesPerWord)
           + (i * map->targetArrayElementSizeInBytes), map->arrayElementType))
      {
        return false;
      }
    }
  }

  return true;
}

void
copy(Thread* t, object typeMaps, object p, uint8_t* dst)
{
  TypeMap* map = typeMap(t, typeMaps, p);
  
  uint8_t* src = reinterpret_cast<uint8_t*>(p);

  for (unsigned i = 0; i < map->fixedFieldCount; ++i) {
    Field* field = map->fixedFields() + i;
    if (field->type > Type_array) abort(t);
    copy(t, src + field->offset, dst + field->targetOffset, field->type);
  }

  if (map->targetArrayElementSizeInBytes) {
    unsigned fixedSize = map->buildFixedSizeInWords * BytesPerWord;
    unsigned count = cast<uintptr_t>(p, fixedSize - BytesPerWord);

    for (unsigned i = 0; i < count; ++i) {
      copy(t, src + fixedSize + (i * map->buildArrayElementSizeInBytes),
           dst + (map->targetFixedSizeInWords * TargetBytesPerWord)
           + (i * map->targetArrayElementSizeInBytes), map->arrayElementType);
    }
  } else {
    switch (map->kind) {
    case TypeMap::NormalKind:
      break;

    case TypeMap::SingletonKind: {
      uint8_t* mask = dst + (map->targetFixedSizeInWords * TargetBytesPerWord);
      memset(mask, 0, singletonMaskSize
             (map->targetFixedSizeInWords - 2, TargetBitsPerWord)
             * TargetBytesPerWord);

      for (unsigned i = 0; i < map->fixedFieldCount; ++i) {
        Field* field = map->fixedFields() + i;
        if (field->type == Type_object) {
          unsigned offset = field->targetOffset / TargetBytesPerWord;
          reinterpret_cast<uint32_t*>(mask)[offset / 32]
            |= static_cast<uint32_t>(1) << (offset % 32);
        }
      }

      if (DebugNativeTarget) {
        expect
          (t, memcmp
           (src + (map->targetFixedSizeInWords * TargetBytesPerWord), mask,
            singletonMaskSize
            (map->targetFixedSizeInWords - 2, TargetBitsPerWord)
            * TargetBytesPerWord) == 0);
      }
    } break;

    case TypeMap::PoolKind: {
      unsigned poolMaskSize = vm::poolMaskSize
        (map->targetFixedSizeInWords - 2, TargetBitsPerWord);

      uint8_t* poolMask = dst
        + (map->targetFixedSizeInWords * TargetBytesPerWord);

      memset(poolMask, 0, poolMaskSize * TargetBytesPerWord);

      uint8_t* objectMask = dst
        + ((map->targetFixedSizeInWords + poolMaskSize) * TargetBytesPerWord);

      memset(objectMask, 0, singletonMaskSize
             (map->targetFixedSizeInWords - 2 + poolMaskSize,
              TargetBitsPerWord) * TargetBytesPerWord);

      for (unsigned i = 0; i < map->fixedFieldCount; ++i) {
        Field* field = map->fixedFields() + i;
        switch (field->type) {
        case Type_object:
          reinterpret_cast<uint32_t*>(objectMask)[i / 32]
            |= static_cast<uint32_t>(1) << (i % 32);
          break;

        case Type_float:
        case Type_double:
          reinterpret_cast<target_uintptr_t*>(poolMask)
            [i / TargetBitsPerWord]
            |= static_cast<target_uintptr_t>(1) << (i % TargetBitsPerWord);
          break;

        default:
          break;
        }
      }

      if (DebugNativeTarget) {
        expect
          (t, memcmp
           (src + (map->targetFixedSizeInWords * TargetBytesPerWord), poolMask,
            (poolMaskSize + singletonMaskSize
             (map->targetFixedSizeInWords - 2 + poolMaskSize,
              TargetBitsPerWord))
            * TargetBytesPerWord) == 0);
      }
    } break;

    default: abort(t);
    }
  }

  if (DebugNativeTarget) {
    expect(t, targetSize(t, typeMaps, p) == baseSize(t, p, objectClass(t, p)));
    expect(t, nonObjectsEqual(map, src, dst));
  }
}

HeapWalker*
makeHeapImage(Thread* t, BootImage* image, uintptr_t* heap, uintptr_t* map,
              unsigned capacity, object constants, object typeMaps)
{
  class Visitor: public HeapVisitor {
   public:
    Visitor(Thread* t, object typeMaps, uintptr_t* heap,
            uintptr_t* map, unsigned capacity):
      t(t), typeMaps(typeMaps), currentObject(0), currentNumber(0),
      currentOffset(0), heap(heap), map(map), position(0), capacity(capacity)
    { }

    void visit(unsigned number) {
      if (currentObject) {
        if (DebugNativeTarget) {
          expect
            (t, targetOffset
             (t, typeMaps, currentObject, currentOffset * BytesPerWord)
             == currentOffset * BytesPerWord);
        }

        unsigned offset = currentNumber - 1
          + (targetOffset
             (t, typeMaps, currentObject, currentOffset * BytesPerWord)
             / TargetBytesPerWord);

        unsigned mark = heap[offset] & (~PointerMask);
        unsigned value = number | (mark << BootShift);

        if (value) markBit(map, offset);

        heap[offset] = value;
      }
    }

    virtual void root() {
      currentObject = 0;
    }

    virtual unsigned visitNew(object p) {
      if (p) {
        unsigned size = targetSize(t, typeMaps, p);

        unsigned number;
        if ((currentObject
             and objectClass(t, currentObject) == type(t, Machine::ClassType)
             and (currentOffset * BytesPerWord) == ClassStaticTable)
            or instanceOf(t, type(t, Machine::SystemClassLoaderType), p))
        {
          // Static tables and system classloaders must be allocated
          // as fixed objects in the heap image so that they can be
          // marked as dirty and visited during GC.  Otherwise,
          // attempts to update references in these objects to point
          // to runtime-allocated memory would fail because we don't
          // scan non-fixed objects in the heap image during GC.

          target_uintptr_t* dst = heap + position + TargetFixieSizeInWords;

          unsigned maskSize = ceiling(size, TargetBytesPerWord);

          unsigned total = TargetFixieSizeInWords + size + maskSize;

          expect(t, position + total < capacity);

          memset(heap + position, 0, TargetFixieSizeInBytes);

          uint8_t age = FixieTenureThreshold + 1;
          memcpy(reinterpret_cast<uint8_t*>(heap + position)
                 + TargetFixieAge, &age, 1);

          uint8_t hasMask = true;
          memcpy(reinterpret_cast<uint8_t*>(heap + position)
                 + TargetFixieHasMask, &hasMask, 1);

          uint32_t targetSize = TARGET_V4(size);
          memcpy(reinterpret_cast<uint8_t*>(heap + position)
                 + TargetFixieSize, &targetSize, 4);

          copy(t, typeMaps, p, reinterpret_cast<uint8_t*>(dst));

          dst[0] |= FixedMark;

          memset(heap + position + TargetFixieSizeInWords + size, 0,
                 maskSize * TargetBytesPerWord);

          number = (dst - heap) + 1;
          position += total;
        } else {
          expect(t, position + size < capacity);

          copy(t, typeMaps, p, reinterpret_cast<uint8_t*>(heap + position));

          number = position + 1;
          position += size;
        }

        visit(number);

        return number;
      } else {
        return 0;
      }
    }

    virtual void visitOld(object, unsigned number) {
      visit(number);
    }

    virtual void push(object object, unsigned number, unsigned offset) {
      currentObject = object;
      currentNumber = number;
      currentOffset = offset;
    }

    virtual void pop() {
      currentObject = 0;
    }

    Thread* t;
    object typeMaps;
    object currentObject;
    unsigned currentNumber;
    unsigned currentOffset;
    target_uintptr_t* heap;
    target_uintptr_t* map;
    unsigned position;
    unsigned capacity;
  } visitor(t, typeMaps, heap, map, capacity / TargetBytesPerWord);

  HeapWalker* w = makeHeapWalker(t, &visitor);
  visitRoots(t, image, w, constants);
  
  image->heapSize = visitor.position * BytesPerWord;

  return w;
}

void
updateConstants(Thread* t, object constants, uint8_t* code, uintptr_t* codeMap,
                HeapMap* heapTable)
{
  for (; constants; constants = tripleThird(t, constants)) {
    unsigned target = heapTable->find(tripleFirst(t, constants));
    expect(t, target > 0);

    for (Promise::Listener* pl = static_cast<ListenPromise*>
           (pointerValue(t, tripleSecond(t, constants)))->listener;
         pl; pl = pl->next)
    {
      void* location;
      bool flat = pl->resolve(0, &location);
      target_uintptr_t offset = target | BootHeapOffset;
      if (flat) {
        offset |= BootFlatConstant;
      }
      memcpy(location, &offset, TargetBytesPerWord);

      expect(t, reinterpret_cast<intptr_t>(location)
             >= reinterpret_cast<intptr_t>(code));

      markBit(codeMap, reinterpret_cast<intptr_t>(location)
              - reinterpret_cast<intptr_t>(code));
    }
  }
}

unsigned
offset(object a, uintptr_t* b)
{
  return reinterpret_cast<uintptr_t>(b) - reinterpret_cast<uintptr_t>(a);
}

void
writeBootImage2(Thread* t, FILE* out, BootImage* image, uint8_t* code,
                unsigned codeCapacity, const char* className,
                const char* methodName, const char* methodSpec)
{
  Zone zone(t->m->system, t->m->heap, 64 * 1024);

  uintptr_t* codeMap = static_cast<uintptr_t*>
    (t->m->heap->allocate(codeMapSize(codeCapacity)));
  memset(codeMap, 0, codeMapSize(codeCapacity));

  object classPoolMap;
  object typeMaps;
  object constants;

  { classPoolMap = makeHashMap(t, 0, 0);
    PROTECT(t, classPoolMap);

    setRoot(t, Machine::PoolMap, classPoolMap);

    typeMaps = makeHashMap(t, 0, 0);
    PROTECT(t, typeMaps);

    constants = makeCodeImage
      (t, &zone, image, code, codeMap, className, methodName, methodSpec,
       typeMaps);

    PROTECT(t, constants);

#include "type-maps.cpp"

    for (unsigned i = 0; i < arrayLength(t, t->m->types); ++i) {
      Type* source = types[i];
      unsigned count = 0;
      while (source[count] != Type_none) {
        ++ count;
      }
      ++ count;

      Type types[count];
      types[0] = Type_object;
      unsigned buildOffsets[count];
      buildOffsets[0] = 0;
      unsigned buildOffset = BytesPerWord;
      unsigned targetOffsets[count];
      targetOffsets[0] = 0;
      unsigned targetOffset = TargetBytesPerWord;
      bool sawArray = false;
      unsigned buildSize = BytesPerWord;
      unsigned targetSize = TargetBytesPerWord;
      for (unsigned j = 1; j < count; ++j) {
        switch (source[j - 1]) {
        case Type_object:
          types[j] = Type_object;
          buildSize = BytesPerWord;
          targetSize = TargetBytesPerWord;
          break;

        case Type_word:
        case Type_intptr_t:
        case Type_uintptr_t:
          types[j] = Type_intptr_t;
          buildSize = BytesPerWord;
          targetSize = TargetBytesPerWord;
          break;

        case Type_int8_t:
        case Type_uint8_t:
          types[j] = Type_int8_t;
          buildSize = targetSize = 1;
          break;

        case Type_int16_t:
        case Type_uint16_t:
          types[j] = Type_int16_t;
          buildSize = targetSize = 2;
          break;

        case Type_int32_t:
        case Type_uint32_t:
        case Type_float:
          types[j] = Type_int32_t;
          buildSize = targetSize = 4;
          break;

        case Type_int64_t:
        case Type_uint64_t:
        case Type_double:
          types[j] = Type_int64_t;
          buildSize = targetSize = 8;
          break;

        case Type_array:
          types[j] = Type_none;
          buildSize = targetSize = 0;
          break;

        default: abort(t);
        }

        if (source[j - 1] == Type_array) {
          sawArray = true;
        }

        if (not sawArray) {
          while (buildOffset % buildSize) {
            ++ buildOffset;
          }

          buildOffsets[j] = buildOffset;

          buildOffset += buildSize;

          while (targetOffset % targetSize) {
            ++ targetOffset;
          }

          targetOffsets[j] = targetOffset;

          targetOffset += targetSize;
        }
      }

      unsigned fixedFieldCount;
      Type arrayElementType;
      unsigned buildArrayElementSize;
      unsigned targetArrayElementSize;
      if (sawArray) {
        fixedFieldCount = count - 2;
        arrayElementType = types[count - 1];
        buildArrayElementSize = buildSize; 
        targetArrayElementSize = targetSize;
      } else {
        fixedFieldCount = count;
        arrayElementType = Type_none;
        buildArrayElementSize = 0;
        targetArrayElementSize = 0;
      }

      object array = makeByteArray
        (t, TypeMap::sizeInBytes
         (ceiling(buildOffset, BytesPerWord), fixedFieldCount));

      TypeMap* map = new (&byteArrayBody(t, array, 0)) TypeMap
        (ceiling(buildOffset, BytesPerWord),
         ceiling(targetOffset, TargetBytesPerWord),
         fixedFieldCount, TypeMap::NormalKind, buildArrayElementSize,
         targetArrayElementSize, arrayElementType);

      for (unsigned j = 0; j < fixedFieldCount; ++j) {
        expect(t, buildOffsets[j] < map->buildFixedSizeInWords * BytesPerWord);

        map->targetFixedOffsets()[buildOffsets[j]] = targetOffsets[j];

        new (map->fixedFields() + j) Field
          (types[j], buildOffsets[j], targetOffsets[j]);
      }

      hashMapInsertOrReplace
        (t, typeMaps, type(t, static_cast<Machine::Type>(i)), array,
         objectHash, objectEqual);
    }

    // these roots will not be used when the bootimage is loaded, so
    // there's no need to preserve them:
    setRoot(t, Machine::PoolMap, 0);
    setRoot(t, Machine::ByteArrayMap, makeWeakHashMap(t, 0, 0));

    // name all primitive classes so we don't try to update immutable
    // references at runtime:
    { object name = makeByteArray(t, "void");
      set(t, type(t, Machine::JvoidType), ClassName, name);
    
      name = makeByteArray(t, "boolean");
      set(t, type(t, Machine::JbooleanType), ClassName, name);

      name = makeByteArray(t, "byte");
      set(t, type(t, Machine::JbyteType), ClassName, name);

      name = makeByteArray(t, "short");
      set(t, type(t, Machine::JshortType), ClassName, name);

      name = makeByteArray(t, "char");
      set(t, type(t, Machine::JcharType), ClassName, name);

      name = makeByteArray(t, "int");
      set(t, type(t, Machine::JintType), ClassName, name);

      name = makeByteArray(t, "float");
      set(t, type(t, Machine::JfloatType), ClassName, name);

      name = makeByteArray(t, "long");
      set(t, type(t, Machine::JlongType), ClassName, name);

      name = makeByteArray(t, "double");
      set(t, type(t, Machine::JdoubleType), ClassName, name);
    }

    // resolve primitive array classes in case they are needed at
    // runtime:
    { object name = makeByteArray(t, "[B");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);

      name = makeByteArray(t, "[Z");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);

      name = makeByteArray(t, "[S");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);

      name = makeByteArray(t, "[C");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);

      name = makeByteArray(t, "[I");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);

      name = makeByteArray(t, "[J");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);

      name = makeByteArray(t, "[F");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);

      name = makeByteArray(t, "[D");
      resolveSystemClass(t, root(t, Machine::BootLoader), name, true);
    }
  }

  uintptr_t* heap = static_cast<uintptr_t*>
    (t->m->heap->allocate(HeapCapacity));
  uintptr_t* heapMap = static_cast<uintptr_t*>
    (t->m->heap->allocate(heapMapSize(HeapCapacity)));
  memset(heapMap, 0, heapMapSize(HeapCapacity));

  HeapWalker* heapWalker = makeHeapImage
    (t, image, heap, heapMap, HeapCapacity, constants, typeMaps);

  updateConstants(t, constants, code, codeMap, heapWalker->map());

  image->bootClassCount = hashMapSize
    (t, classLoaderMap(t, root(t, Machine::BootLoader)));

  unsigned* bootClassTable = static_cast<unsigned*>
    (t->m->heap->allocate(image->bootClassCount * sizeof(unsigned)));

  { unsigned i = 0;
    for (HashMapIterator it
           (t, classLoaderMap(t, root(t, Machine::BootLoader)));
         it.hasMore();)
    {
      bootClassTable[i++] = heapWalker->map()->find
        (tripleSecond(t, it.next()));
    }
  }

  image->appClassCount = hashMapSize
    (t, classLoaderMap(t, root(t, Machine::AppLoader)));

  unsigned* appClassTable = static_cast<unsigned*>
    (t->m->heap->allocate(image->appClassCount * sizeof(unsigned)));

  { unsigned i = 0;
    for (HashMapIterator it
           (t, classLoaderMap(t, root(t, Machine::AppLoader)));
         it.hasMore();)
    {
      appClassTable[i++] = heapWalker->map()->find(tripleSecond(t, it.next()));
    }
  }

  image->stringCount = hashMapSize(t, root(t, Machine::StringMap));
  unsigned* stringTable = static_cast<unsigned*>
    (t->m->heap->allocate(image->stringCount * sizeof(unsigned)));

  { unsigned i = 0;
    for (HashMapIterator it(t, root(t, Machine::StringMap)); it.hasMore();) {
      stringTable[i++] = heapWalker->map()->find
        (jreferenceTarget(t, tripleFirst(t, it.next())));
    }
  }

  unsigned* callTable = t->m->processor->makeCallTable(t, heapWalker);

  heapWalker->dispose();

  image->magic = BootImage::Magic;
  image->codeBase = reinterpret_cast<uintptr_t>(code);

  fprintf(stderr, "class count %d string count %d call count %d\n"
          "heap size %d code size %d\n",
          image->bootClassCount, image->stringCount, image->callCount,
          image->heapSize, image->codeSize);

  if (true) {
    fwrite(image, sizeof(BootImage), 1, out);

    fwrite(bootClassTable, image->bootClassCount * sizeof(unsigned), 1, out);
    fwrite(appClassTable, image->appClassCount * sizeof(unsigned), 1, out);
    fwrite(stringTable, image->stringCount * sizeof(unsigned), 1, out);
    fwrite(callTable, image->callCount * sizeof(unsigned) * 2, 1, out);

    unsigned offset = (image->bootClassCount * sizeof(unsigned))
      + (image->appClassCount * sizeof(unsigned))
      + (image->stringCount * sizeof(unsigned))
      + (image->callCount * sizeof(unsigned) * 2);

    while (offset % TargetBytesPerWord) {
      uint8_t c = 0;
      fwrite(&c, 1, 1, out);
      ++ offset;
    }

    fwrite(heapMap, pad(heapMapSize(image->heapSize)), 1, out);
    fwrite(heap, pad(image->heapSize), 1, out);

    fwrite(codeMap, pad(codeMapSize(image->codeSize)), 1, out);
    fwrite(code, pad(image->codeSize), 1, out);
  }
}

uint64_t
writeBootImage(Thread* t, uintptr_t* arguments)
{
  FILE* out = reinterpret_cast<FILE*>(arguments[0]);
  BootImage* image = reinterpret_cast<BootImage*>(arguments[1]);
  uint8_t* code = reinterpret_cast<uint8_t*>(arguments[2]);
  unsigned codeCapacity = arguments[3];
  const char* className = reinterpret_cast<const char*>(arguments[4]);
  const char* methodName = reinterpret_cast<const char*>(arguments[5]);
  const char* methodSpec = reinterpret_cast<const char*>(arguments[6]);

  writeBootImage2
    (t, out, image, code, codeCapacity, className, methodName, methodSpec);

  return 1;
}

} // namespace

int
main(int ac, const char** av)
{
  if (ac < 3 or ac > 6) {
    fprintf(stderr, "usage: %s <classpath> <output file> "
            "[<class name> [<method name> [<method spec>]]]\n", av[0]);
    return -1;
  }

  System* s = makeSystem(0);
  Heap* h = makeHeap(s, HeapCapacity * 2);
  Classpath* c = makeClasspath(s, h, AVIAN_JAVA_HOME, AVIAN_EMBED_PREFIX);
  Finder* f = makeFinder(s, h, av[1], 0);
  Processor* p = makeProcessor(s, h, false);

  // todo: currently, the compiler cannot compile code with jumps or
  // calls spanning more than the maximum size of an immediate value
  // in a branch instruction for the target architecture (~32MB on
  // PowerPC and ARM).  When that limitation is removed, we'll be able
  // to specify a capacity as large as we like here:
  const unsigned CodeCapacity = 30 * 1024 * 1024;

  uint8_t* code = static_cast<uint8_t*>(h->allocate(CodeCapacity));
  BootImage image;
  p->initialize(&image, code, CodeCapacity);

  Machine* m = new (h->allocate(sizeof(Machine))) Machine
    (s, h, f, 0, p, c, 0, 0, 0, 0);
  Thread* t = p->makeThread(m, 0, 0);
  
  enter(t, Thread::ActiveState);
  enter(t, Thread::IdleState);

  FILE* output = vm::fopen(av[2], "wb");
  if (output == 0) {
    fprintf(stderr, "unable to open %s\n", av[2]);    
    return -1;
  }

  uintptr_t arguments[] = { reinterpret_cast<uintptr_t>(output),
                            reinterpret_cast<uintptr_t>(&image),
                            reinterpret_cast<uintptr_t>(code),
                            CodeCapacity,
                            reinterpret_cast<uintptr_t>(ac > 3 ? av[3] : 0),
                            reinterpret_cast<uintptr_t>(ac > 4 ? av[4] : 0),
                            reinterpret_cast<uintptr_t>(ac > 5 ? av[5] : 0) };

  run(t, writeBootImage, arguments);

  fclose(output);

  if (t->exception) {
    printTrace(t, t->exception);
    return -1;
  } else {
    return 0;
  }
}
