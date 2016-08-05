#include <DB/Storages/MergeTree/MergeTreePartChecker.h>
#include <DB/DataStreams/MarkInCompressedFile.h>
#include <DB/DataTypes/DataTypeString.h>
#include <DB/DataTypes/DataTypeDate.h>
#include <DB/DataTypes/DataTypeDateTime.h>
#include <DB/DataTypes/DataTypesNumberFixed.h>
#include <DB/DataTypes/DataTypeFixedString.h>
#include <DB/DataTypes/DataTypeAggregateFunction.h>
#include <DB/DataTypes/DataTypeArray.h>
#include <DB/DataTypes/DataTypeNested.h>
#include <DB/IO/CompressedReadBuffer.h>
#include <DB/IO/HashingReadBuffer.h>
#include <DB/Columns/ColumnsNumber.h>
#include <DB/Common/CurrentMetrics.h>
#include <DB/Common/escapeForFileName.h>


namespace DB
{

namespace ErrorCodes
{
	extern const int CORRUPTED_DATA;
	extern const int INCORRECT_MARK;
	extern const int EMPTY_LIST_OF_COLUMNS_PASSED;
}


namespace
{

constexpr auto DATA_FILE_EXTENSION = ".bin";
constexpr auto NULL_MAP_EXTENSION = ".null";
constexpr auto MARKS_FILE_EXTENSION = ".mrk";
constexpr auto NULL_MARKS_FILE_EXTENSION = ".null_mrk";

/// bin / mrk
/// null / null_mrk

struct Stream
{
public:
	Stream(const String & path, const String & name, const DataTypePtr & type,
	       const std::string & extension_, const std::string & mrk_extension_)
		: path(path), name(name), type(type),
		extension{extension_}, mrk_extension{mrk_extension_},
		file_buf(path + name + extension), compressed_hashing_buf(file_buf),
		uncompressing_buf(compressed_hashing_buf),
		uncompressed_hashing_buf(uncompressing_buf),
		mrk_file_buf(path + name + mrk_extension),
		mrk_hashing_buf(mrk_file_buf)
	{
		/// Stream создаётся для типа - внутренностей массива. Случай, когда внутренность массива - массив - не поддерживается.
		if (typeid_cast<const DataTypeArray *>(type.get()))
			throw Exception("Multidimensional arrays are not supported", ErrorCodes::NOT_IMPLEMENTED);
	}

	bool marksEOF()
	{
		return mrk_hashing_buf.eof();
	}

	void ignore()
	{
		uncompressed_hashing_buf.ignore(std::numeric_limits<size_t>::max());
		mrk_hashing_buf.ignore(std::numeric_limits<size_t>::max());
	}

	size_t read(size_t rows)
	{
		ColumnPtr column = type->createColumn();
		type->deserializeBinary(*column, uncompressed_hashing_buf, rows, 0);
		return column->size();
	}

	size_t readUInt64(size_t rows, ColumnUInt64::Container_t & data)
	{
		if (data.size() < rows)
			data.resize(rows);
		size_t size = uncompressed_hashing_buf.readBig(reinterpret_cast<char *>(&data[0]), sizeof(UInt64) * rows);
		if (size % sizeof(UInt64))
			throw Exception("Read " + toString(size) + " bytes, which is not divisible by " + toString(sizeof(UInt64)),
				ErrorCodes::CORRUPTED_DATA);
		return size / sizeof(UInt64);
	}

	void assertMark()
	{
		MarkInCompressedFile mrk_mark;
		readIntBinary(mrk_mark.offset_in_compressed_file, mrk_hashing_buf);
		readIntBinary(mrk_mark.offset_in_decompressed_block, mrk_hashing_buf);

		bool has_alternative_mark = false;
		MarkInCompressedFile alternative_data_mark;
		MarkInCompressedFile data_mark;

		/// Если засечка должна быть ровно на границе блоков, нам подходит и засечка, указывающая на конец предыдущего блока,
		///  и на начало следующего.
		if (!uncompressed_hashing_buf.hasPendingData())
		{
			/// Получим засечку, указывающую на конец предыдущего блока.
			has_alternative_mark = true;
			alternative_data_mark.offset_in_compressed_file = compressed_hashing_buf.count() - uncompressing_buf.getSizeCompressed();
			alternative_data_mark.offset_in_decompressed_block = uncompressed_hashing_buf.offset();

			if (mrk_mark == alternative_data_mark)
				return;

			uncompressed_hashing_buf.next();

			/// В конце файла compressed_hashing_buf.count() указывает на конец файла даже до вызова next(),
			///  и только что выполненная проверка работает неправильно. Для простоты не будем проверять последнюю засечку.
			if (uncompressed_hashing_buf.eof())
				return;
		}

		data_mark.offset_in_compressed_file = compressed_hashing_buf.count() - uncompressing_buf.getSizeCompressed();
		data_mark.offset_in_decompressed_block = uncompressed_hashing_buf.offset();

		if (mrk_mark != data_mark)
			throw Exception("Incorrect mark: " + data_mark.toString() +
				(has_alternative_mark ? " or " + alternative_data_mark.toString() : "") + " in data, " +
				mrk_mark.toString() + " in " + mrk_extension + " file", ErrorCodes::INCORRECT_MARK);
	}

	void assertEnd(MergeTreeData::DataPart::Checksums & checksums)
	{
		if (!uncompressed_hashing_buf.eof())
			throw Exception("EOF expected in column data", ErrorCodes::CORRUPTED_DATA);
		if (!mrk_hashing_buf.eof())
			throw Exception("EOF expected in .mrk file", ErrorCodes::CORRUPTED_DATA);

		checksums.files[name + extension] = MergeTreeData::DataPart::Checksums::Checksum(
			compressed_hashing_buf.count(), compressed_hashing_buf.getHash(),
			uncompressed_hashing_buf.count(), uncompressed_hashing_buf.getHash());
		checksums.files[name + mrk_extension] = MergeTreeData::DataPart::Checksums::Checksum(
			mrk_hashing_buf.count(), mrk_hashing_buf.getHash());
	}

public:
	String path;
	String name;
	DataTypePtr type;
	std::string extension;
	std::string mrk_extension;

	ReadBufferFromFile file_buf;
	HashingReadBuffer compressed_hashing_buf;
	CompressedReadBuffer uncompressing_buf;
	HashingReadBuffer uncompressed_hashing_buf;

	ReadBufferFromFile mrk_file_buf;
	HashingReadBuffer mrk_hashing_buf;
};

/// Returns the number of rows. Updates the "checksums" variable with the checksum of
/// each column's null byte map file.
size_t checkNullableColumn(const String & path,
	const String & name,
	const MergeTreePartChecker::Settings & settings,
	MergeTreeData::DataPart::Checksums & checksums,
	volatile bool * is_cancelled)
{
	size_t rows = 0;

	DataTypePtr type = std::make_shared<DataTypeUInt8>();
	Stream data_stream(path, escapeForFileName(name), type,
		NULL_MAP_EXTENSION, NULL_MARKS_FILE_EXTENSION);

	while (true)
	{
		if (is_cancelled && *is_cancelled)
			return 0;

		if (data_stream.marksEOF())
			break;

		data_stream.assertMark();

		size_t cur_rows = data_stream.read(settings.index_granularity);

		rows += cur_rows;
		if (cur_rows < settings.index_granularity)
			break;
	}

	data_stream.assertEnd(checksums);

	return rows;
}

/// Returns the number of rows. Updates the "checksums" variable with the checksum of
/// each column's bin file.
size_t checkColumn(
	const String & path,
	const String & name,
	DataTypePtr type,
	const MergeTreePartChecker::Settings & settings,
	MergeTreeData::DataPart::Checksums & checksums,
	std::atomic<bool> * is_cancelled)
{
	size_t rows = 0;

	try
	{
		if (auto array = typeid_cast<const DataTypeArray *>(type.get()))
		{
			String sizes_name = DataTypeNested::extractNestedTableName(name);
			Stream sizes_stream(path, escapeForFileName(sizes_name) + ".size0", std::make_shared<DataTypeUInt64>(),
				DATA_FILE_EXTENSION, MARKS_FILE_EXTENSION);
			Stream data_stream(path, escapeForFileName(name), array->getNestedType(),
				DATA_FILE_EXTENSION, MARKS_FILE_EXTENSION);

			ColumnUInt64::Container_t sizes;
			while (true)
			{
				if (is_cancelled && *is_cancelled)
					return 0;

				if (sizes_stream.marksEOF())
					break;

				sizes_stream.assertMark();
				data_stream.assertMark();

				size_t cur_rows = sizes_stream.readUInt64(settings.index_granularity, sizes);

				size_t sum = 0;
				for (size_t i = 0; i < cur_rows; ++i)
				{
					size_t new_sum = sum + sizes[i];
					if (sizes[i] > (1ul << 31) || new_sum < sum)
						throw Exception("Array size " + toString(sizes[i]) + " is too long.", ErrorCodes::CORRUPTED_DATA);
					sum = new_sum;
				}

				data_stream.read(sum);

				rows += cur_rows;
				if (cur_rows < settings.index_granularity)
					break;
			}

			sizes_stream.assertEnd(checksums);
			data_stream.assertEnd(checksums);

			return rows;
		}
		else
		{
			Stream data_stream(path, escapeForFileName(name), type,
				DATA_FILE_EXTENSION, MARKS_FILE_EXTENSION);

			size_t rows = 0;
			while (true)
			{
				if (is_cancelled && *is_cancelled)
					return 0;

				if (data_stream.marksEOF())
					break;

				data_stream.assertMark();

				size_t cur_rows = data_stream.read(settings.index_granularity);

				rows += cur_rows;
				if (cur_rows < settings.index_granularity)
					break;
			}

			data_stream.assertEnd(checksums);

			return rows;
		}
	}
	catch (Exception & e)
	{
		e.addMessage(" (column: " + path + name + ", last mark at " + toString(rows) + " rows)");
		throw;
	}
}

}


void MergeTreePartChecker::checkDataPart(
	String path,
	const Settings & settings,
	const DataTypes & primary_key_data_types,
	MergeTreeData::DataPart::Checksums * out_checksums,
	std::atomic<bool> * is_cancelled)
{
	CurrentMetrics::Increment metric_increment{CurrentMetrics::ReplicatedChecks};

	if (!path.empty() && path.back() != '/')
		path += "/";

	NamesAndTypesList columns;

	/// Чексуммы из файла checksums.txt. Могут отсутствовать. Если присутствуют - впоследствии сравниваются с реальными чексуммами данных.
	MergeTreeData::DataPart::Checksums checksums_txt;

	{
		ReadBufferFromFile buf(path + "columns.txt");
		columns.readText(buf);
		assertEOF(buf);
	}

	if (settings.require_checksums || Poco::File(path + "checksums.txt").exists())
	{
		ReadBufferFromFile buf(path + "checksums.txt");
		checksums_txt.read(buf);
		assertEOF(buf);
	}

	/// Реальные чексуммы по содержимому данных. Их несоответствие checksums_txt будет говорить о битых данных.
	MergeTreeData::DataPart::Checksums checksums_data;

	size_t marks_in_primary_key = 0;
	{
		ReadBufferFromFile file_buf(path + "primary.idx");
		HashingReadBuffer hashing_buf(file_buf);

		if (!primary_key_data_types.empty())
		{
			size_t key_size = primary_key_data_types.size();
			Columns tmp_columns(key_size);

			for (size_t j = 0; j < key_size; ++j)
				tmp_columns[j] = primary_key_data_types[j].get()->createColumn();

			while (!hashing_buf.eof())
			{
				if (is_cancelled && *is_cancelled)
					return;

				++marks_in_primary_key;
				for (size_t j = 0; j < key_size; ++j)
					primary_key_data_types[j].get()->deserializeBinary(*tmp_columns[j].get(), hashing_buf);
			}
		}
		else
		{
			hashing_buf.tryIgnore(std::numeric_limits<size_t>::max());
		}

		size_t primary_idx_size = hashing_buf.count();

		checksums_data.files["primary.idx"] = MergeTreeData::DataPart::Checksums::Checksum(primary_idx_size, hashing_buf.getHash());
	}

	if (is_cancelled && *is_cancelled)
		return;

	String any_column_name;

	static constexpr size_t UNKNOWN = std::numeric_limits<size_t>::max();

	size_t rows = UNKNOWN;
	std::exception_ptr first_exception;

	for (const NameAndTypePair & column : columns)
	{
		if (settings.verbose)
		{
			std::cerr << column.name << ":";
			std::cerr.flush();
		}

		bool ok = false;
		try
		{
			if (!settings.require_column_files && !Poco::File(path + escapeForFileName(column.name) + ".bin").exists())
			{
				if (settings.verbose)
					std::cerr << " no files" << std::endl;
				continue;
			}

			size_t cur_rows = checkColumn(path, column.name, column.type, settings, checksums_data, is_cancelled);

			if (is_cancelled && *is_cancelled)
				return;

			if (rows == UNKNOWN)
			{
				rows = cur_rows;
				any_column_name = column.name;
			}
			else if (rows != cur_rows)
			{
				throw Exception("Different number of rows in columns " + any_column_name + " and " + column.name,
								ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);
			}

			if (column.type.get()->isNullable())
			{
				size_t row_count_from_null_map = checkNullableColumn(path,
					column.name, settings, checksums_data, is_cancelled);
				if (row_count_from_null_map != rows)
					throw Exception{"Inconsistent number of rows in null byte map for column " + column.name,
									ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH};
			}

			ok = true;
		}
		catch (...)
		{
			if (!settings.verbose)
				throw;

			std::exception_ptr e = std::current_exception();
			if (!first_exception)
				first_exception = e;

			std::cerr << getCurrentExceptionMessage(true) << std::endl;
		}

		if (settings.verbose && ok)
			std::cerr << " ok" << std::endl;
	}

	if (rows == UNKNOWN)
		throw Exception("No columns", ErrorCodes::EMPTY_LIST_OF_COLUMNS_PASSED);

	if (!primary_key_data_types.empty())
	{
		const size_t expected_marks = (rows - 1) / settings.index_granularity + 1;
		if (expected_marks != marks_in_primary_key)
			throw Exception("Size of primary key doesn't match expected number of marks."
				" Number of rows in columns: " + toString(rows)
				+ ", index_granularity: " + toString(settings.index_granularity)
				+ ", expected number of marks: " + toString(expected_marks)
				+ ", size of primary key: " + toString(marks_in_primary_key),
				ErrorCodes::CORRUPTED_DATA);
	}

	if (settings.require_checksums || !checksums_txt.files.empty())
		checksums_txt.checkEqual(checksums_data, true);

	if (first_exception)
		std::rethrow_exception(first_exception);

	if (out_checksums)
		*out_checksums = checksums_data;
}

}
