// Class TAPValidator checks TAP file (DataInterchange structure) validity according to TD.57 requirements.
// If Fatal or Severe errors are found it creates RAP file (Return Batch structure) and registers it in DB tables (RAP_File, RAP_Fatal_Return and so on).

#include <math.h>
#include <set>
#include "OTL_Header.h"
#include "TAP_Constants.h"
#include "DataInterchange.h"
#include "BatchControlInfo.h"
#include "ReturnBatch.h"
#include "ConfigContainer.h"
#include "TAPValidator.h"

using namespace std;

extern void log(string filename, short msgType, string msgText);
extern void log(short msgType, string msgText);
extern long long OctetStr2Int64(const OCTET_STRING_t& octetStr);
extern int LoadReturnBatchToDB(ReturnBatch* returnBatch, long fileID, string rapFilename, long fileStatus);
extern int write_out(const void *buffer, size_t size, void *app_key);
extern "C" int ncftp_main(int argc, char **argv, char* result);


RAPFile::RAPFile(otl_connect& dbConnect, Config& config) 
	: m_otlConnect(dbConnect), m_config(config)
{
}


int RAPFile::OctetString_fromInt64(OCTET_STRING& octetStr, long long value)
{
	unsigned char buf[8];
	int i;
	// fill buffer with value, most significant bytes first, less significant - last
	for (i = 7; i >= 0; i--) {
		buf[i] = value & 0xFF;
		value >>= 8;
		if (value == 0) break;
	}
	if (i == 0 && value > 0)
		throw "8-byte integer overflow";

	if (buf[i] >= 0x80) {
		// it will be treated as negative value, so add one more byte
		if (i == 0)
			throw "8-byte integer overflow";
		buf[--i] = 0;
	}

	OCTET_STRING_fromBuf(&octetStr, (const char*) ( buf + i ), 8 - i);

	return 8 - i;
}


bool RAPFile::UploadFileToFtp(string filename, string fullFileName, FtpSetting ftpSetting)
{
	try {
		if (ftpSetting.ftpPort.length() == 0)
			ftpSetting.ftpPort = "21";	// use default ftp port
		int ncftp_argc = 11;
		const char* pszArguments[] = { "ncftpput", "-u", ftpSetting.ftpUsername.c_str(), "-p", 
			ftpSetting.ftpPassword.c_str(), "-P", ftpSetting.ftpPort.c_str(), ftpSetting.ftpServer.c_str(), 
			ftpSetting.ftpDirectory.c_str(), fullFileName.c_str(), NULL };
		char szFtpResult[4096];
		int ftpResult = ncftp_main(ncftp_argc, (char**) pszArguments, szFtpResult);
		if (ftpResult != 0) {
			log(filename, LOG_ERROR, "Error while uploading file " + filename + " on FTP server " + ftpSetting.ftpServer + ":");
			log(filename, LOG_ERROR, szFtpResult);
			return false;
		}
		log(filename, LOG_INFO, "Successful upload to FTP server " + ftpSetting.ftpServer);
		return true;
	}
	catch (...) {
		log(filename, LOG_ERROR, "Exception while uploading " + filename + " to FTP server " 
			+ ftpSetting.ftpServer + ". Uploading failed.");
		return false;
	}
}


int RAPFile::EncodeAndUpload(ReturnBatch* returnBatch, string filename, string roamingHubName)
{
	string fullFileName;
#ifdef WIN32
	fullFileName = (m_config.GetOutputDirectory().empty() ? "." : m_config.GetOutputDirectory()) + "\\" + filename;
#else
	fullFileName = (m_config.GetOutputDirectory().empty() ? "." : m_config.GetOutputDirectory()) + "/" + filename;
#endif

	FILE *fTapFile = fopen(fullFileName.c_str(), "wb");
	if (!fTapFile) {
		log(filename, LOG_ERROR, string("Unable to open file ") + fullFileName + " for writing.");
		return TL_FILEERROR;
	}
	asn_enc_rval_t encodeRes = der_encode(&asn_DEF_ReturnBatch, returnBatch, write_out, fTapFile);

	fclose(fTapFile);

	if (encodeRes.encoded == -1) {
		log(filename, LOG_ERROR, string("Error while encoding ASN file. Error code ") + 
			string(encodeRes.failed_type ? encodeRes.failed_type->name : "unknown"));
		return TL_DECODEERROR;
	}

	log(filename, LOG_INFO, "RAP file successfully created for roaming hub " + roamingHubName);

	// Upload file to FTP-server
	FtpSetting ftpSetting = m_config.GetFTPSetting(roamingHubName);
	if (!ftpSetting.ftpServer.empty()) {
		if (!UploadFileToFtp(filename, fullFileName, ftpSetting)) {
			return TL_FILEERROR;
		}
	}
	else
		log(filename, LOG_INFO, "FTP server is not set in cfg-file for roaming hub " + roamingHubName + ". No uploading done.");

	return TL_OK;
}


int RAPFile::CreateRAPFile(ReturnBatch* returnBatch, ReturnDetail* returnDetail, string sender, string recipient, 
	string tapAvailableStamp, string fileTypeIndicator, long& rapFileID, string& rapSequenceNum)
{
	otl_nocommit_stream otlStream;
	otlStream.open(1, "call BILLING.TAP3.CreateRAPFileByTAPLoader(:pRecipientTAPCode /*char[10],in*/,"
		":pTestData /*long,in*/, to_date(:pDate /*char[30],in*/, 'yyyymmddhh24miss'), :pRAPFilename /*char[50],out*/, "
		":pRAPSequenceNum /*char[10],out*/, :pMobileNetworkID /*long,out*/, :pRoamingHubID /*long,out*/, :pRoamingHubName /*char[100],out*/,"
		":pTimestamp /*char[20],out*/, :pUTCOffset /*char[10],out*/, :pTAPVersion /*long,out*/, :pTAPRelease /*long,out*/, "
		":pRAPVersion /*long,out*/, :pRAPRelease /*long,out*/, :pTapDecimalPlaces /*long,out*/)"
		" into :fileid /*long,out*/" , m_otlConnect);
	otlStream
		<< recipient
		<< (long) (fileTypeIndicator.size()>0 ? 1 : 0)
		<< tapAvailableStamp;
	
	string filename, roamingHubName, timeStamp, utcOffset;
	long  mobileNetworkID, roamingHubID, tapVersion, tapRelease, rapVersion, rapRelease, tapDecimalPlaces;
	otlStream
		>> filename
		>> rapSequenceNum
		>> mobileNetworkID
		>> roamingHubID
		>> roamingHubName
		>> timeStamp
		>> utcOffset
		>> tapVersion
		>> tapRelease
		>> rapVersion
		>> rapRelease
		>> tapDecimalPlaces
		>> rapFileID;
	
	// sender and recipient switch their places
	OCTET_STRING_fromBuf(&returnBatch->rapBatchControlInfoRap.sender, sender.c_str(), sender.size());
	OCTET_STRING_fromBuf(&returnBatch->rapBatchControlInfoRap.recipient, recipient.c_str(), recipient.size());

	OCTET_STRING_fromBuf(&returnBatch->rapBatchControlInfoRap.rapFileSequenceNumber, rapSequenceNum.c_str(), rapSequenceNum.size());
	returnBatch->rapBatchControlInfoRap.rapFileCreationTimeStamp.localTimeStamp =
		OCTET_STRING_new_fromBuf (&asn_DEF_LocalTimeStamp, timeStamp.c_str(), timeStamp.size());
	returnBatch->rapBatchControlInfoRap.rapFileCreationTimeStamp.utcTimeOffset =
		OCTET_STRING_new_fromBuf (&asn_DEF_UtcTimeOffset, utcOffset.c_str(), utcOffset.size());
	returnBatch->rapBatchControlInfoRap.rapFileAvailableTimeStamp.localTimeStamp =
		OCTET_STRING_new_fromBuf (&asn_DEF_LocalTimeStamp, timeStamp.c_str(), timeStamp.size());
	returnBatch->rapBatchControlInfoRap.rapFileAvailableTimeStamp.utcTimeOffset =
		OCTET_STRING_new_fromBuf (&asn_DEF_UtcTimeOffset, utcOffset.c_str(), utcOffset.size());

	returnBatch->rapBatchControlInfoRap.tapDecimalPlaces = (TapDecimalPlaces_t*) calloc(1, sizeof(TapDecimalPlaces_t));
	*returnBatch->rapBatchControlInfoRap.tapDecimalPlaces = tapDecimalPlaces;
	
	returnBatch->rapBatchControlInfoRap.rapSpecificationVersionNumber = rapVersion;
	returnBatch->rapBatchControlInfoRap.rapReleaseVersionNumber = rapRelease;
	returnBatch->rapBatchControlInfoRap.specificationVersionNumber = (SpecificationVersionNumber_t*) calloc(1, sizeof(SpecificationVersionNumber_t));
	*returnBatch->rapBatchControlInfoRap.specificationVersionNumber = tapVersion;
	returnBatch->rapBatchControlInfoRap.releaseVersionNumber = (ReleaseVersionNumber_t*) calloc(1, sizeof(ReleaseVersionNumber_t));
	*returnBatch->rapBatchControlInfoRap.releaseVersionNumber = tapRelease;
			
	if (!fileTypeIndicator.empty())
		returnBatch->rapBatchControlInfoRap.fileTypeIndicator = 
			OCTET_STRING_new_fromBuf(&asn_DEF_FileTypeIndicator, fileTypeIndicator.c_str(), fileTypeIndicator.size());
		
// TODO: Operator specific info is mandatory for IOT errors (TD.52 RAP implementation handbook)
// Fill it for Severe errors
			
	ASN_SEQUENCE_ADD(&returnBatch->returnDetails, returnDetail);

	OctetString_fromInt64(returnBatch->rapAuditControlInfo.totalSevereReturnValue, (long long) 0);
	returnBatch->rapAuditControlInfo.returnDetailsCount = 1; // For Fatal errors 

	int loadResult = LoadReturnBatchToDB(returnBatch, rapFileID, filename, OUTFILE_CREATED_AND_SENT);
	if (loadResult >= 0)
		loadResult = EncodeAndUpload(returnBatch, filename, roamingHubName);
	
	return loadResult;
}

//-----------------------------------------------------------

TAPValidator::TAPValidator(otl_connect& dbConnect, Config& config) 
	: m_otlConnect(dbConnect), m_config(config), m_rapFileID(0)
{
}


bool TAPValidator::BatchContainsTaxes()
{
	for (int call_index = 0; call_index < m_transferBatch->callEventDetails->list.count; call_index++) {
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_mobileOriginatedCall) {
			MobileOriginatedCall* moCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.mobileOriginatedCall;
			for (int bs_used_index = 0; bs_used_index < moCall->basicServiceUsedList->list.count; bs_used_index++) 
				for (int chr_index = 0; chr_index < moCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.count; chr_index++)
					if (moCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.array[chr_index]->taxInformation != NULL)
						return true;
		}
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_mobileTerminatedCall) {
			MobileTerminatedCall* mtCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.mobileTerminatedCall;
			for (int bs_used_index = 0; bs_used_index < mtCall->basicServiceUsedList->list.count; bs_used_index++) 
				for (int chr_index = 0; chr_index < mtCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.count; chr_index++)
					if (mtCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.array[chr_index]->taxInformation != NULL)
						return true;
		}
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_gprsCall) {
			GprsCall* gprsCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.gprsCall;
			for (int chr_index = 0; chr_index < gprsCall->gprsServiceUsed->chargeInformationList->list.count; chr_index++)
				if (gprsCall->gprsServiceUsed->chargeInformationList->list.array[chr_index]->taxInformation != NULL)
					return true;
		}
	}
	return false;
}

bool TAPValidator::BatchContainsDiscounts()
{
	for (int call_index = 0; call_index < m_transferBatch->callEventDetails->list.count; call_index++) {
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_mobileOriginatedCall) {
			MobileOriginatedCall* moCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.mobileOriginatedCall;
			for (int bs_used_index = 0; bs_used_index < moCall->basicServiceUsedList->list.count; bs_used_index++) 
				for (int chr_index = 0; chr_index < moCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.count; chr_index++)
					if (moCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.array[chr_index]->discountInformation != NULL)
						return true;
		}
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_mobileTerminatedCall) {
			MobileTerminatedCall* mtCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.mobileTerminatedCall;
			for (int bs_used_index = 0; bs_used_index < mtCall->basicServiceUsedList->list.count; bs_used_index++) 
				for (int chr_index = 0; chr_index < mtCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.count; chr_index++)
					if (mtCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.array[chr_index]->discountInformation != NULL)
						return true;
		}
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_gprsCall) {
			GprsCall* gprsCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.gprsCall;
			for (int chr_index = 0; chr_index < gprsCall->gprsServiceUsed->chargeInformationList->list.count; chr_index++)
				if (gprsCall->gprsServiceUsed->chargeInformationList->list.array[chr_index]->discountInformation != NULL)
					return true;
		}
	}
	return false;
}

bool TAPValidator::ChargeInfoContainsPositiveCharges(ChargeInformation* chargeInfo)
{
	double tapPower = pow( (double) 10, *m_transferBatch->accountingInfo->tapDecimalPlaces);
	for (int chr_det_index = 0; chr_det_index < chargeInfo->chargeDetailList->list.count; chr_det_index++) {
		double charge = OctetStr2Int64(*chargeInfo->chargeDetailList->list.array[chr_det_index]->charge) / tapPower;
		if (charge > 0)
			return true;
	}
	return false;
}

bool TAPValidator::BatchContainsPositiveCharges()
{
	for (int call_index = 0; call_index < m_transferBatch->callEventDetails->list.count; call_index++) {
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_mobileOriginatedCall) {
			MobileOriginatedCall* moCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.mobileOriginatedCall;
			for (int bs_used_index = 0; bs_used_index < moCall->basicServiceUsedList->list.count; bs_used_index++)
				for (int chr_index = 0; chr_index < moCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.count; chr_index++)
					if (ChargeInfoContainsPositiveCharges(
							moCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.array[chr_index]))
						return true;
		}
		
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_mobileTerminatedCall) {
			MobileTerminatedCall* mtCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.mobileTerminatedCall;
			for (int bs_used_index = 0; bs_used_index < mtCall->basicServiceUsedList->list.count; bs_used_index++) 
				for (int chr_index = 0; chr_index < mtCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.count; chr_index++) 
					if (ChargeInfoContainsPositiveCharges(
							mtCall->basicServiceUsedList->list.array[bs_used_index]->chargeInformationList->list.array[chr_index]))
						return true;
		}
		if(m_transferBatch->callEventDetails->list.array[call_index]->present == CallEventDetail_PR_gprsCall) {
			GprsCall* gprsCall = &m_transferBatch->callEventDetails->list.array[call_index]->choice.gprsCall;
			for (int chr_index = 0; chr_index < gprsCall->gprsServiceUsed->chargeInformationList->list.count; chr_index++)
				if (ChargeInfoContainsPositiveCharges(
						gprsCall->gprsServiceUsed->chargeInformationList->list.array[chr_index]))
					return true;
		}
	}
	return false;
}


int TAPValidator::CreateTransferBatchRAPFile(string logMessage, int errorCode)
{
	log(LOG_ERROR, "Validating Transfer Batch: " + logMessage + ". Creating RAP file");
	ReturnDetail* returnDetail = (ReturnDetail*) calloc(1, sizeof(ReturnDetail));
	returnDetail->present = ReturnDetail_PR_fatalReturn;
	OCTET_STRING_fromBuf(&returnDetail->choice.fatalReturn.fileSequenceNumber, (const char*)m_transferBatch->batchControlInfo->fileSequenceNumber->buf, 
		m_transferBatch->batchControlInfo->fileSequenceNumber->size);
	returnDetail->choice.fatalReturn.transferBatchError = (TransferBatchError*) calloc(1, sizeof(TransferBatchError));
	ErrorDetail* errorDetail = (ErrorDetail*) calloc(1, sizeof(ErrorDetail));
	errorDetail->errorCode = errorCode;

	// Fill Error Context List
	errorDetail->errorContext = (ErrorContextList*) calloc(1, sizeof(ErrorContextList));
	ErrorContext* errorContext1level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext1level->pathItemId = asn_DEF_TransferBatch.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it
	errorContext1level->itemLevel = 1;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext1level);
	ASN_SEQUENCE_ADD(&returnDetail->choice.fatalReturn.transferBatchError->errorDetail, errorDetail);
	
	ReturnBatch* returnBatch = (ReturnBatch*) calloc(1, sizeof(ReturnBatch));
	RAPFile rapFile(m_otlConnect, m_config);
	
	assert(m_transferBatch->batchControlInfo->sender);
	assert(m_transferBatch->batchControlInfo->recipient);
	assert(m_transferBatch->batchControlInfo->fileAvailableTimeStamp);
	int loadRes = rapFile.CreateRAPFile(returnBatch, returnDetail, (char*)m_transferBatch->batchControlInfo->sender->buf,
		(char*) m_transferBatch->batchControlInfo->recipient->buf, (char*) m_transferBatch->batchControlInfo->fileAvailableTimeStamp->localTimeStamp->buf,
		(m_transferBatch->batchControlInfo->fileTypeIndicator ? (char*) m_transferBatch->batchControlInfo->fileTypeIndicator->buf : ""),
		m_rapFileID, m_rapSequenceNum);
	ASN_STRUCT_FREE(asn_DEF_ReturnBatch, returnBatch);
	return loadRes;
}


int TAPValidator::CreateBatchControlInfoRAPFile(string logMessage, int errorCode)
{
	log(LOG_ERROR, "Validating Batch Control Info: " + logMessage + ". Creating RAP file ");
	ReturnDetail* returnDetail = (ReturnDetail*) calloc(1, sizeof(ReturnDetail));
	returnDetail->present = ReturnDetail_PR_fatalReturn;
	OCTET_STRING_fromBuf(&returnDetail->choice.fatalReturn.fileSequenceNumber, 
		(const char*) m_transferBatch->batchControlInfo->fileSequenceNumber->buf, 
		m_transferBatch->batchControlInfo->fileSequenceNumber->size);
	returnDetail->choice.fatalReturn.batchControlError = (BatchControlError*) calloc(1, sizeof(BatchControlError));
	
	//Copy batchControlInfo fields to Return Batch structure
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.sender =
		m_transferBatch->batchControlInfo->sender;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.recipient = 
		m_transferBatch->batchControlInfo->recipient;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileAvailableTimeStamp = 
		m_transferBatch->batchControlInfo->fileAvailableTimeStamp;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileCreationTimeStamp = 
		m_transferBatch->batchControlInfo->fileCreationTimeStamp;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.transferCutOffTimeStamp = 
		m_transferBatch->batchControlInfo->transferCutOffTimeStamp;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileSequenceNumber = 
		m_transferBatch->batchControlInfo->fileSequenceNumber;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileTypeIndicator = 
		m_transferBatch->batchControlInfo->fileTypeIndicator;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.operatorSpecInformation = 
		m_transferBatch->batchControlInfo->operatorSpecInformation;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.rapFileSequenceNumber = 
		m_transferBatch->batchControlInfo->rapFileSequenceNumber;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.releaseVersionNumber = 
		m_transferBatch->batchControlInfo->releaseVersionNumber;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.specificationVersionNumber = 
		m_transferBatch->batchControlInfo->specificationVersionNumber;
	
	ErrorDetail* errorDetail = (ErrorDetail*) calloc(1, sizeof(ErrorDetail));
	errorDetail->errorCode = errorCode;

	// Fill Error Context List
	errorDetail->errorContext = (ErrorContextList*) calloc(1, sizeof(ErrorContextList));
	ErrorContext* errorContext1level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext1level->pathItemId = asn_DEF_TransferBatch.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext1level->itemLevel = 1;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext1level);

	ErrorContext* errorContext2level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext2level->pathItemId = asn_DEF_BatchControlInfo.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext2level->itemLevel = 2;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext2level);
	ASN_SEQUENCE_ADD(&returnDetail->choice.fatalReturn.batchControlError->errorDetail, errorDetail);

	ReturnBatch* returnBatch = (ReturnBatch*) calloc(1, sizeof(ReturnBatch));
	RAPFile rapFile(m_otlConnect, m_config);
	
	assert(m_transferBatch->batchControlInfo->sender);
	assert(m_transferBatch->batchControlInfo->recipient);
	assert(m_transferBatch->batchControlInfo->fileAvailableTimeStamp);
	int loadRes = rapFile.CreateRAPFile(returnBatch, returnDetail, (char*)m_transferBatch->batchControlInfo->sender->buf,
		(char*) m_transferBatch->batchControlInfo->recipient->buf, (char*) m_transferBatch->batchControlInfo->fileAvailableTimeStamp->localTimeStamp->buf,
		(m_transferBatch->batchControlInfo->fileTypeIndicator ? (char*) m_transferBatch->batchControlInfo->fileTypeIndicator->buf : ""),
		m_rapFileID, m_rapSequenceNum);
	
	// Clear previously copied pointers to avoid ASN_STRUCT_FREE errors
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.sender = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.recipient = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileAvailableTimeStamp = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileCreationTimeStamp = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.transferCutOffTimeStamp = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileSequenceNumber = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.fileTypeIndicator = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.operatorSpecInformation = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.rapFileSequenceNumber = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.releaseVersionNumber = NULL;
	returnDetail->choice.fatalReturn.batchControlError->batchControlInfo.specificationVersionNumber = NULL;
	ASN_STRUCT_FREE(asn_DEF_ReturnBatch, returnBatch);

	return loadRes;
}


int TAPValidator::CreateAccountingInfoRAPFile(string logMessage, int errorCode, asn_TYPE_descriptor_t* level3item)
{
	log(LOG_ERROR, "Validating Accounting Info: " + logMessage + ". Creating RAP file");
	ReturnDetail* returnDetail = (ReturnDetail*) calloc(1, sizeof(ReturnDetail));
	returnDetail->present = ReturnDetail_PR_fatalReturn;
	OCTET_STRING_fromBuf(&returnDetail->choice.fatalReturn.fileSequenceNumber, (const char*) m_transferBatch->batchControlInfo->fileSequenceNumber->buf, 
		m_transferBatch->batchControlInfo->fileSequenceNumber->size);
	returnDetail->choice.fatalReturn.accountingInfoError = (AccountingInfoError*) calloc(1, sizeof(AccountingInfoError));
	
	//Copy AccountingInfo fields to Return Batch structure
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.currencyConversionInfo = 
		m_transferBatch->accountingInfo->currencyConversionInfo;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.discounting = 
		m_transferBatch->accountingInfo->discounting;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.localCurrency = 
		m_transferBatch->accountingInfo->localCurrency;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.tapCurrency = 
		m_transferBatch->accountingInfo->tapCurrency;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.tapDecimalPlaces = 
		m_transferBatch->accountingInfo->tapDecimalPlaces;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.taxation = 
		m_transferBatch->accountingInfo->taxation;
		
	ErrorDetail* errorDetail = (ErrorDetail*) calloc(1, sizeof(ErrorDetail));
	errorDetail->errorCode = errorCode;

	// Fill Error Context List
	errorDetail->errorContext = (ErrorContextList*) calloc(1, sizeof(ErrorContextList));
	ErrorContext* errorContext1level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext1level->pathItemId = asn_DEF_TransferBatch.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext1level->itemLevel = 1;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext1level);

	ErrorContext* errorContext2level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext2level->pathItemId = asn_DEF_AccountingInfo.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext2level->itemLevel = 2;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext2level);
	
	if (level3item) {
		ErrorContext* errorContext3level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
		errorContext3level->pathItemId = level3item->tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
		errorContext3level->itemLevel = 3;
		ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext3level);
	}
	ASN_SEQUENCE_ADD(&returnDetail->choice.fatalReturn.accountingInfoError->errorDetail, errorDetail);

	ReturnBatch* returnBatch = (ReturnBatch*) calloc(1, sizeof(ReturnBatch));
	RAPFile rapFile(m_otlConnect, m_config);
	
	assert(m_transferBatch->batchControlInfo->sender);
	assert(m_transferBatch->batchControlInfo->recipient);
	assert(m_transferBatch->batchControlInfo->fileAvailableTimeStamp);
	int loadRes = rapFile.CreateRAPFile(returnBatch, returnDetail, (char*)m_transferBatch->batchControlInfo->sender->buf,
		(char*) m_transferBatch->batchControlInfo->recipient->buf, (char*) m_transferBatch->batchControlInfo->fileAvailableTimeStamp->localTimeStamp->buf,
		(m_transferBatch->batchControlInfo->fileTypeIndicator ? (char*) m_transferBatch->batchControlInfo->fileTypeIndicator->buf : ""),
		m_rapFileID, m_rapSequenceNum);
	
	// Clear previously copied pointers to avoid ASN_STRUCT_FREE errors
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.currencyConversionInfo = NULL;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.discounting = NULL;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.localCurrency = NULL;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.tapCurrency = NULL;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.tapDecimalPlaces = NULL;
	returnDetail->choice.fatalReturn.accountingInfoError->accountingInfo.taxation = NULL;
	ASN_STRUCT_FREE(asn_DEF_ReturnBatch, returnBatch);

	return loadRes;
}


int TAPValidator::CreateNetworkInfoRAPFile(string logMessage, int errorCode)
{
	log(LOG_ERROR, "Validating Network Info: " + logMessage + ". Creating RAP file");
	ReturnDetail* returnDetail = (ReturnDetail*) calloc(1, sizeof(ReturnDetail));
	returnDetail->present = ReturnDetail_PR_fatalReturn;
	OCTET_STRING_fromBuf(&returnDetail->choice.fatalReturn.fileSequenceNumber, (const char*) m_transferBatch->batchControlInfo->fileSequenceNumber->buf, 
		m_transferBatch->batchControlInfo->fileSequenceNumber->size);
	returnDetail->choice.fatalReturn.networkInfoError = (NetworkInfoError*) calloc(1, sizeof(NetworkInfoError));
	
	//Copy NetworkInfo fields to Return Batch structure
	returnDetail->choice.fatalReturn.networkInfoError->networkInfo.recEntityInfo = 
		m_transferBatch->networkInfo->recEntityInfo;
	returnDetail->choice.fatalReturn.networkInfoError->networkInfo.utcTimeOffsetInfo = 
		m_transferBatch->networkInfo->utcTimeOffsetInfo;
		
	ErrorDetail* errorDetail = (ErrorDetail*) calloc(1, sizeof(ErrorDetail));
	errorDetail->errorCode = errorCode;

	// Fill Error Context List
	errorDetail->errorContext = (ErrorContextList*) calloc(1, sizeof(ErrorContextList));
	ErrorContext* errorContext1level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext1level->pathItemId = asn_DEF_TransferBatch.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext1level->itemLevel = 1;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext1level);

	ErrorContext* errorContext2level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext2level->pathItemId = asn_DEF_NetworkInfo.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext2level->itemLevel = 2;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext2level);
	ASN_SEQUENCE_ADD(&returnDetail->choice.fatalReturn.networkInfoError->errorDetail, errorDetail);

	ReturnBatch* returnBatch = (ReturnBatch*) calloc(1, sizeof(ReturnBatch));
	RAPFile rapFile(m_otlConnect, m_config);
	
	assert(m_transferBatch->batchControlInfo->sender);
	assert(m_transferBatch->batchControlInfo->recipient);
	assert(m_transferBatch->batchControlInfo->fileAvailableTimeStamp);
	int loadRes = rapFile.CreateRAPFile(returnBatch, returnDetail, (char*)m_transferBatch->batchControlInfo->sender->buf,
		(char*) m_transferBatch->batchControlInfo->recipient->buf, (char*) m_transferBatch->batchControlInfo->fileAvailableTimeStamp->localTimeStamp->buf,
		(m_transferBatch->batchControlInfo->fileTypeIndicator ? (char*) m_transferBatch->batchControlInfo->fileTypeIndicator->buf : ""),
		m_rapFileID, m_rapSequenceNum);
	
	// Clear previously copied pointers to avoid ASN_STRUCT_FREE errors
	returnDetail->choice.fatalReturn.networkInfoError->networkInfo.recEntityInfo = NULL;
	returnDetail->choice.fatalReturn.networkInfoError->networkInfo.utcTimeOffsetInfo = NULL;
	ASN_STRUCT_FREE(asn_DEF_ReturnBatch, returnBatch);

	return loadRes;
}


int TAPValidator::CreateAuditControlInfoRAPFile(string logMessage, int errorCode, asn_TYPE_descriptor_t* level3item)
{
	log(LOG_ERROR, "Validating Audit Control Info: " + logMessage + ". Creating RAP file");
	ReturnDetail* returnDetail = (ReturnDetail*) calloc(1, sizeof(ReturnDetail));
	returnDetail->present = ReturnDetail_PR_fatalReturn;
	OCTET_STRING_fromBuf(&returnDetail->choice.fatalReturn.fileSequenceNumber, (const char*) m_transferBatch->batchControlInfo->fileSequenceNumber->buf, 
		m_transferBatch->batchControlInfo->fileSequenceNumber->size);
	returnDetail->choice.fatalReturn.auditControlInfoError = (AuditControlInfoError*) calloc(1, sizeof(AuditControlInfoError));
	
	//Copy auditControlInfo fields to Return Batch structure
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.callEventDetailsCount = 
		m_transferBatch->auditControlInfo->callEventDetailsCount;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.earliestCallTimeStamp = 
		m_transferBatch->auditControlInfo->earliestCallTimeStamp;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.latestCallTimeStamp = 
		m_transferBatch->auditControlInfo->latestCallTimeStamp;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.operatorSpecInformation = 
		m_transferBatch->auditControlInfo->operatorSpecInformation;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalAdvisedChargeValueList = 
		m_transferBatch->auditControlInfo->totalAdvisedChargeValueList;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalCharge = 
		m_transferBatch->auditControlInfo->totalCharge;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalChargeRefund = 
		m_transferBatch->auditControlInfo->totalChargeRefund;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalDiscountRefund = 
		m_transferBatch->auditControlInfo->totalDiscountRefund;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalDiscountValue = 
		m_transferBatch->auditControlInfo->totalDiscountValue;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalTaxRefund = 
		m_transferBatch->auditControlInfo->totalTaxRefund;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalTaxValue = 
		m_transferBatch->auditControlInfo->totalTaxValue;

	ErrorDetail* errorDetail = (ErrorDetail*) calloc(1, sizeof(ErrorDetail));
	errorDetail->errorCode = errorCode;

	// Fill Error Context List
	errorDetail->errorContext = (ErrorContextList*) calloc(1, sizeof(ErrorContextList));
	ErrorContext* errorContext1level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext1level->pathItemId = asn_DEF_TransferBatch.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext1level->itemLevel = 1;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext1level);

	ErrorContext* errorContext2level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
	errorContext2level->pathItemId = asn_DEF_AuditControlInfo.tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
	errorContext2level->itemLevel = 2;
	ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext2level);
	
	if (level3item) {
		ErrorContext* errorContext3level = (ErrorContext*) calloc(1, sizeof(ErrorContext));
		errorContext3level->pathItemId = level3item->tags[0] >> 2; // 2 rightmost bits is TAG class at ASN1 compiler, remove it  
		errorContext3level->itemLevel = 3;
		ASN_SEQUENCE_ADD(errorDetail->errorContext, errorContext3level);
	}
	ASN_SEQUENCE_ADD(&returnDetail->choice.fatalReturn.auditControlInfoError->errorDetail, errorDetail);

	ReturnBatch* returnBatch = (ReturnBatch*) calloc(1, sizeof(ReturnBatch));
	RAPFile rapFile(m_otlConnect, m_config);
	
	assert(m_transferBatch->batchControlInfo->sender);
	assert(m_transferBatch->batchControlInfo->sender);
	assert(m_transferBatch->batchControlInfo->fileAvailableTimeStamp);
	int loadRes = rapFile.CreateRAPFile(returnBatch, returnDetail, (char*)m_transferBatch->batchControlInfo->sender->buf,
		(char*) m_transferBatch->batchControlInfo->recipient->buf, (char*) m_transferBatch->batchControlInfo->fileAvailableTimeStamp->localTimeStamp->buf,
		(m_transferBatch->batchControlInfo->fileTypeIndicator ? (char*) m_transferBatch->batchControlInfo->fileTypeIndicator->buf : ""),
		m_rapFileID, m_rapSequenceNum);
	
	// Clear previously copied pointers to avoid ASN_STRUCT_FREE errors
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.callEventDetailsCount = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.earliestCallTimeStamp = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.latestCallTimeStamp = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.operatorSpecInformation = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalAdvisedChargeValueList = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalCharge = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalChargeRefund = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalDiscountRefund = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalDiscountValue = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalTaxRefund = NULL;
	returnDetail->choice.fatalReturn.auditControlInfoError->auditControlInfo.totalTaxValue = NULL;
	ASN_STRUCT_FREE(asn_DEF_ReturnBatch, returnBatch);
	
	return loadRes;
}


TAPValidationResult TAPValidator::ValidateBatchControlInfo()
{
	if (!m_transferBatch->batchControlInfo->sender || !m_transferBatch->batchControlInfo->recipient || 
			!m_transferBatch->batchControlInfo->fileSequenceNumber) {
		log(LOG_ERROR, "Validation: Sender, Recipient or FileSequenceNumber is missing in Batch Control Info. Unable to create RAP file.");
		return VALIDATION_IMPOSSIBLE;
	}

	if (!m_transferBatch->batchControlInfo->fileAvailableTimeStamp) {
		int createRapRes = CreateBatchControlInfoRAPFile("fileAvailableTimeStamp is missing in Batch Control Info", 
			BATCH_CTRL_FILE_AVAIL_TIMESTAMP_MISSING);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->batchControlInfo->specificationVersionNumber) {
		int createRapRes = CreateBatchControlInfoRAPFile("specificationVersionNumber is missing in Batch Control Info", 
			BATCH_CTRL_SPEC_VERSION_MISSING);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->batchControlInfo->transferCutOffTimeStamp) {
		int createRapRes = CreateBatchControlInfoRAPFile("transferCutOffTimeStamp is missing in Batch Control Info", 
			BATCH_CTRL_TRANSFER_CUTOFF_MISSING);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	return TAP_VALID;
}

TAPValidationResult TAPValidator::ValidateAccountingInfo()
{
	if (!m_transferBatch->accountingInfo->localCurrency) {
		int createRapRes = CreateAccountingInfoRAPFile("localCurrency is missing in Accounting Info", 
			ACCOUNTING_LOCAL_CURRENCY_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->accountingInfo->tapDecimalPlaces) {
		int createRapRes = CreateAccountingInfoRAPFile("tapDecimalPlaces is missing in Accounting Info", 
			ACCOUNTING_TAP_DECIMAL_PLACES_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->accountingInfo->taxation && BatchContainsTaxes()) {
		int createRapRes = CreateAccountingInfoRAPFile(
			"taxation group is missing in Accounting Info and batch contains taxes", 
			ACCOUNTING_TAXATION_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->accountingInfo->discounting && BatchContainsDiscounts()) {
		int createRapRes = CreateAccountingInfoRAPFile(
			"discounting group is missing in Accounting Info and batch contains discounts", 
			ACCOUNTING_DISCOUNTING_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->accountingInfo->currencyConversionInfo && BatchContainsPositiveCharges()) {
		int createRapRes = CreateAccountingInfoRAPFile(
			"currencyConversion group is missing in Accounting Info and batch contains charges greater than 0",
			ACCOUNTING_CURRENCY_CONVERSION_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	// Validating currency conversion table
	if (m_transferBatch->accountingInfo->currencyConversionInfo) {
		set<ExchangeRateCode_t> exchangeRateCodes;
		for (int i = 0; i < m_transferBatch->accountingInfo->currencyConversionInfo->list.count; i++) {
			if (!m_transferBatch->accountingInfo->currencyConversionInfo->list.array[i]->exchangeRateCode) {
				int createRapRes = CreateAccountingInfoRAPFile(
					"Mandatory item Exchange Rate Code missing within group Currency Conversion",
					CURRENCY_CONVERSION_EXRATE_CODE_MISSING, &asn_DEF_CurrencyConversionList);
				return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
			}
			if (!m_transferBatch->accountingInfo->currencyConversionInfo->list.array[i]->numberOfDecimalPlaces) {
				int createRapRes = CreateAccountingInfoRAPFile(
					"Mandatory item Exchange Rate Code missing within group Currency Conversion",
					CURRENCY_CONVERSION_NUM_OF_DEC_PLACES_MISSING, &asn_DEF_CurrencyConversionList);
				return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
			}
			if (!m_transferBatch->accountingInfo->currencyConversionInfo->list.array[i]->exchangeRate) {
				int createRapRes = CreateAccountingInfoRAPFile(
					"Mandatory item Exchange Rate Code missing within group Currency Conversion",
					CURRENCY_CONVERSION_EXCHANGE_RATE_MISSING, &asn_DEF_CurrencyConversionList);
				return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
			}
			// check exchange rate code duplication
			if (exchangeRateCodes.find(*m_transferBatch->accountingInfo->currencyConversionInfo->list.array[i]->exchangeRateCode) !=
					exchangeRateCodes.end()) {
				int createRapRes = CreateAccountingInfoRAPFile(
					"More than one occurrence of group with same Exchange Rate Code within group Currency Conversion",
					CURRENCY_CONVERSION_EXRATE_CODE_DUPLICATION, &asn_DEF_CurrencyConversionList);
				return ( createRapRes >= 0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE );
			}
			else {
				exchangeRateCodes.insert(*m_transferBatch->accountingInfo->currencyConversionInfo->list.array[i]->exchangeRateCode);
			}
		}
	}

	return TAP_VALID;
}


TAPValidationResult TAPValidator::ValidateNetworkInfo()
{
	// check mandatory structures in Transfer Batch/Network Information
	if (!m_transferBatch->networkInfo->utcTimeOffsetInfo) {
		int createRapRes = CreateNetworkInfoRAPFile("utcTimeOffsetInfo is missing in Network Info",
			NETWORK_UTC_TIMEOFFSET_MISSING);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->networkInfo->recEntityInfo) {
		int createRapRes = CreateNetworkInfoRAPFile("recEntityInfo is missing in Network Info",
			NETWORK_REC_ENTITY_MISSING);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	return TAP_VALID;
}


TAPValidationResult TAPValidator::ValidateAuditControlInfo()
{
	// check mandatory structures in Transfer Batch/Audit Control Information
	if (!m_transferBatch->auditControlInfo->totalCharge) {
		int createRapRes = CreateAuditControlInfoRAPFile("totalCharge is missing in Audit Control Info",
			AUDIT_CTRL_TOTAL_CHARGE_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->auditControlInfo->totalTaxValue) {
		int createRapRes = CreateAuditControlInfoRAPFile("totalTaxValue is missing in Audit Control Info",
			AUDIT_CTRL_TOTAL_TAX_VALUE_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->auditControlInfo->totalDiscountValue) {
		int createRapRes = CreateAuditControlInfoRAPFile("totalDiscountValue is missing in Audit Control Info",
			AUDIT_CTRL_TOTAL_DISCOUNT_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->auditControlInfo->callEventDetailsCount) {
		int createRapRes = CreateAuditControlInfoRAPFile("callEventDetailsCount is missing in Audit Control Info",
			AUDIT_CTRL_CALL_COUNT_MISSING, NULL);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (*m_transferBatch->auditControlInfo->callEventDetailsCount != m_transferBatch->callEventDetails->list.count) {
		int createRapRes = CreateAuditControlInfoRAPFile(
			"Audit Control Info/CallEventDetailsCount does not match the count of Call Event Details",
			CALL_COUNT_MISMATCH, &asn_DEF_CallEventDetailsCount);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	return TAP_VALID;
}


TAPValidationResult TAPValidator::ValidateTransferBatch()
{
	// check mandatory structures in Transfer Batch
	if (!m_transferBatch->batchControlInfo) {
		int createRapRes = CreateTransferBatchRAPFile("Batch Control Info missing in Transfer Batch", 
			TF_BATCH_BATCH_CONTROL_INFO_MISSING);
		return (createRapRes >=0 ? FATAL_ERROR : VALIDATION_IMPOSSIBLE);
	}
	if (!m_transferBatch->accountingInfo) {
		int createRapRes = CreateTransferBatchRAPFile("Accounting Info missing in Transfer Batch", 
			TF_BATCH_ACCOUNTING_INFO_MISSING);
		return FATAL_ERROR;
	}
	if (!m_transferBatch->networkInfo) {
		int createRapRes = CreateTransferBatchRAPFile("Network Info missing in Transfer Batch", 
			TF_BATCH_NETWORK_INFO_MISSING);
		return FATAL_ERROR;
	}
	if (!m_transferBatch->auditControlInfo) {
		int createRapRes = CreateTransferBatchRAPFile("Audit Control Info missing in Transfer Batch", 
			TF_BATCH_AUDIT_CONTROL_INFO_MISSING);
		return FATAL_ERROR;
	}
	
	// Validating transfer batch structures
	TAPValidationResult validationRes = ValidateBatchControlInfo();
	if (validationRes != TL_OK)
		return validationRes;

	validationRes = ValidateAccountingInfo();
	if (validationRes != TL_OK)
		return validationRes;

	// check mandatory structures in Transfer Batch/Network Information
	validationRes = ValidateNetworkInfo();
	if (validationRes != TL_OK)
		return validationRes;

	validationRes = ValidateAuditControlInfo();
	if (validationRes != TL_OK)
		return validationRes;
	
	return TAP_VALID;
}


TAPValidationResult TAPValidator::ValidateNotification()
{
	// check mandatory structures in Transfer Batch/Batch Control Information
	if (!m_notification->sender || !m_notification->recipient || !m_notification->fileSequenceNumber) {
		log(LOG_ERROR, "Validation: Sender, Recipient or FileSequenceNumber is missing in Notification. Unable to create RAP file.");
		return VALIDATION_IMPOSSIBLE;
	}

	return TAP_VALID;
}


TAPValidationResult TAPValidator::Validate(DataInterChange* dataInterchange)
{
	// TODO: add check
	// if file is not addressed for our network, then return WRONG_ADDRESSEE;
	switch (dataInterchange->present) {
		case DataInterChange_PR_transferBatch:
			m_transferBatch = &dataInterchange->choice.transferBatch;
			m_notification = NULL;
			return ValidateTransferBatch();
		case DataInterChange_PR_notification:
			m_notification = &dataInterchange->choice.notification;
			m_transferBatch = NULL;
			return ValidateNotification();
		default:
			return VALIDATION_IMPOSSIBLE;
	}
}

long TAPValidator::GetRapFileID()
{
	return m_rapFileID;
}

string TAPValidator::GetRapSequenceNum()
{
	return m_rapSequenceNum;
}