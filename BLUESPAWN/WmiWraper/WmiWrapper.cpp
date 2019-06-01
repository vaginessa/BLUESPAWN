#include "stdafx.h"
#include "WmiWrapper.h"
#include <sstream>

// TODO: rename strange variables, such as all the IWbemClassObjects

namespace WmiWrapper {
	// Creates a reference to a IWbemServices instance with proper security permission
	WmiWrapper::WmiWrapper() {
		//Clean old instances
		try {
			CoUninitialize();
		}
		catch (...) {}

		HRESULT hres;
		// Initialize COM
		hres = CoInitializeEx(0, COINIT_MULTITHREADED);
		if (FAILED(hres)) {
			std::cerr << "Failed to initialize COM library. Error code = 0x" << std::hex << hres << std::endl;
			exit(1);
		}

		// Set general COM security levels
		hres = CoInitializeSecurity(
			NULL,
			-1,                          // COM authentication
			NULL,                        // Authentication services
			NULL,                        // Reserved
			RPC_C_AUTHN_LEVEL_DEFAULT,   // Default authentication 
			RPC_C_IMP_LEVEL_IMPERSONATE, // Default Impersonation  
			NULL,                        // Authentication info
			EOAC_NONE,                   // Additional capabilities 
			NULL                         // Reserved
		);

		if (FAILED(hres)) {
			std::cerr << "Failed to initialize security. Error code = 0x" << std::hex << hres << std::endl;
			CoUninitialize();
			exit(1);
		}

		// Obtain the initial locator to WMI
		IWbemLocator *pLoc = NULL; //output pointer
		hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&pLoc);

		if (FAILED(hres)) {
			std::cerr << "Failed to create IWbemLocator object."
				<< " Err code = 0x"
				<< std::hex << hres << std::endl;
			CoUninitialize();
			exit(1);
		}

		// Connect to WMI through the IWbemLocator::ConnectServer method
		IWbemServices *pSvc = NULL;

		// Connect to the root\cimv2 namespace with the current user and obtain pointer pSvc
		hres = pLoc->ConnectServer(
			_bstr_t(L"ROOT"),		 // Object path of WMI namespace
			NULL,                    // User name. NULL = current user
			NULL,                    // User password. NULL = current
			0,                       // Locale. NULL indicates current
			NULL,                    // Security flags.
			0,                       // Authority (for example, Kerberos)
			0,                       // Context object 
			&pSvc                    // pointer to IWbemServices proxy
		);

		if (FAILED(hres)) {
			std::cerr << "Could not connect to namespace. Error code = 0x" << std::hex << hres << std::endl;
			pLoc->Release();
			CoUninitialize();
			exit(1);
		}

		std::cout << "Connected to ROOT namespace" << std::endl;


		// Set security levels on the proxy
		hres = CoSetProxyBlanket(
			pSvc,                        // Indicates the proxy to set
			RPC_C_AUTHN_WINNT,           // RPC_C_AUTHN_xxx
			RPC_C_AUTHZ_NONE,            // RPC_C_AUTHZ_xxx
			NULL,                        // Server principal name 
			RPC_C_AUTHN_LEVEL_CALL,      // RPC_C_AUTHN_LEVEL_xxx 
			RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
			NULL,                        // client identity
			EOAC_NONE                    // proxy capabilities 
		);

		if (FAILED(hres)) {
			std::cerr << "Could not set proxy blanket. Error code = 0x" << std::hex << hres << std::endl;
			pSvc->Release();
			pLoc->Release();
			CoUninitialize();
			exit(1);
		}

		this->pSvc = pSvc;

		// Cleanup
		pLoc->Release();
	}

	WmiWrapper::~WmiWrapper() {
		if (pSvc)
			pSvc->Release();

		if (pUnsecApp)
			pUnsecApp->Release();

		CoUninitialize();
	}

	void WmiWrapper::Release() {
		delete this;
	}


	/////////////////////////////////////
	//       Core WMI Functions        //
	/////////////////////////////////////

	std::unique_ptr<std::vector<IWbemClassObject*>> WmiWrapper::retrieveWmiQuerry(const std::string &classType) {
		std::string wmiNamespace = WmiObjectNameParser::getNamespace(classType);
		std::string wmiClassType = WmiObjectNameParser::getClassType(classType);

		// Get pointer to WMI namespace
		IWbemServices *tmpSvc = NULL;
		HRESULT namespaceHr = getServicesToNamespace(wmiNamespace, &tmpSvc);
		if (FAILED(namespaceHr))
			return nullptr;

		auto objects = std::make_unique <std::vector<IWbemClassObject* >> ();
		IEnumWbemClassObject* pEnumerator = NULL;

		// Retrieve Querry
		std::string querry = "SELECT * FROM " + wmiClassType;
		HRESULT querryHr = tmpSvc->ExecQuery(
			bstr_t("WQL"),											// Querry language
			bstr_t(querry.c_str()),									// The querry itself
			WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,	// Suggested flags for top performance
			NULL,													// Optional context information
			&pEnumerator);											// Enumerator for object retrieval. Stores output.

		// Cleanup if querry failed
		if (FAILED(querryHr)) {
			std::cerr << "Query for operating system name failed. Error code = 0x" << std::hex << querryHr << std::endl;

			pEnumerator->Release();
			if (tmpSvc != pSvc)
				tmpSvc->Release();

			return nullptr;
		}

		// Add querry results to vector
		IWbemClassObject* pWmiObj = NULL;
		ULONG uReturn = 0;

		while (pEnumerator) {
			HRESULT iterHr = pEnumerator->Next(WBEM_INFINITE, 1, &pWmiObj, &uReturn);

			if (uReturn == 0)
				break;

			objects->push_back(pWmiObj);
		}

		// Cleanup
		pEnumerator->Release();
		if (tmpSvc != pSvc)
			tmpSvc->Release();

		return objects;
	}

	IWbemObjectSink* WmiWrapper::registerObjectSink(IWbemObjectSink *pSink, const std::string &eventType, const std::string &classType) {
		pSink->AddRef();

		IUnknown* pStubUnk = NULL;
		if (pUnsecApp == NULL) {
			HRESULT hres = CoCreateInstance(CLSID_UnsecuredApartment, NULL,
				CLSCTX_LOCAL_SERVER, IID_IUnsecuredApartment,
				(void**)&pUnsecApp);
		}

		pUnsecApp->CreateObjectStub(pSink, &pStubUnk);
		if (pStubUnk == NULL) {
			std::wcerr << "NULL PSINK" << std::endl;
		}

		IWbemObjectSink* pObjSink = NULL; // output pointer
		pStubUnk->QueryInterface(IID_IWbemObjectSink, (void **)&pObjSink);

		std::string wmiNamespace = WmiObjectNameParser::getNamespace(classType);
		std::string wmiClassType = WmiObjectNameParser::getClassType(classType);

		// Get pointer to WMI namespace
		IWbemServices *tmpSvc = NULL;
		HRESULT namespaceHr = getServicesToNamespace(wmiNamespace, &tmpSvc);
		if (FAILED(namespaceHr))
			return nullptr;

		// Construct asyncronous event querry
		std::string querry = "SELECT * FROM __" + eventType + " WITHIN 1 WHERE TargetInstance ISA '" + wmiClassType + "'";
		HRESULT querryHr = tmpSvc->ExecNotificationQueryAsync(
			_bstr_t("WQL"),					// Querry language
			_bstr_t(querry.c_str()),		// The querry
			WBEM_FLAG_SEND_STATUS,			// Indicates the ObjectSink should recieve intermediate reports through SetStatus
			NULL,							// Otpional pointer to an IWbemContext object to 
			pObjSink);						// Pointer to IWbemObjectSink implementation. Events will go here.


											// Clean up and handle failed querry
		if (tmpSvc != pSvc)
			tmpSvc->Release();
		pStubUnk->Release();

		if (FAILED(querryHr)) {
			std::cerr << "ExecNotificationQueryAsync failed. Error code = 0x" << std::hex << querryHr << std::endl;
			pObjSink->Release();
			pSink->Release();
			return nullptr;
		}

		return pObjSink;
	}

	HRESULT WmiWrapper::getServicesToNamespace(const std::string & wmiNamespace, IWbemServices ** tmpSvc) {
		if (wmiNamespace != "") {
			std::wstring wmiNamespaceWstr(wmiNamespace.begin(), wmiNamespace.end());
			HRESULT hr = pSvc->OpenNamespace(SysAllocStringLen(
				wmiNamespaceWstr.data(), wmiNamespaceWstr.size()),	// Relative namespace as BSTR
				0,													// Flag to make call synchronous
				NULL,												// Reserved: must be null
				tmpSvc,												// Pointer to pointer to new IWbemServices
				NULL);												// Optional pointer to new IWbemCallResult
			if (FAILED(hr)) {
				std::cerr << "Query for namespace " + wmiNamespace + " failed." << " Error code = 0x" << std::hex << hr << std::endl;
				*tmpSvc = nullptr;
			}

			return hr;
		}
		else
			*tmpSvc = pSvc;

		return HRESULT(0);
	}


	///////////////////////////////////////
	//      Property Lists and Maps      //
	///////////////////////////////////////

	WmiWrapper::WmiMap* WmiWrapper::getWmiObjPropMap(IWbemClassObject *pWmiObj) {
		WmiMap *propMap = new WmiMap;

		std::vector<std::wstring>* propKeys = getWmiObjPropKeys(pWmiObj);
		if (propKeys == nullptr) {
			delete propMap;
			return nullptr;
		}
		std::vector<std::unique_ptr<VARIANT>>* propVals = getWmiObjPropVals(pWmiObj, propKeys);

		for (int i = 0; i < propKeys->size(); i++) {
			propMap->insert(std::make_pair(propKeys->at(i), std::move(propVals->at(i))));
			if (propVals->at(i) != nullptr)
				std::cout << "ERROR" << std::endl;
		}

		delete propKeys;
		delete propVals;

		return propMap;
	}

	std::vector<std::wstring>* WmiWrapper::getWmiObjPropKeys(IWbemClassObject* pWmiObj) {
		std::vector<std::wstring>* props = new std::vector<std::wstring>;
		SAFEARRAY *psaNames = NULL;
		HRESULT namesHr = pWmiObj->GetNames(
			NULL,
			WBEM_FLAG_ALWAYS | WBEM_FLAG_NONSYSTEM_ONLY,
			NULL,
			&psaNames);

		if (FAILED(namesHr)) {
			delete props;
			return nullptr;
		}

		// Determine loop bounds
		long lLower, lUpper;
		BSTR PropName = NULL;
		SafeArrayGetLBound(psaNames, 1, &lLower);
		SafeArrayGetUBound(psaNames, 1, &lUpper);

		for (long i = lLower; i <= lUpper; i++) {
			namesHr = SafeArrayGetElement(
				psaNames,
				&i,
				&PropName);

			props->push_back(PropName);

			SysFreeString(PropName);
		}

		// Cleanup
		SafeArrayDestroy(psaNames);

		return props;
	}

	std::vector<std::unique_ptr<VARIANT>>* WmiWrapper::getWmiObjPropVals(IWbemClassObject* pWmiObj, std::vector<std::wstring>* pProps) {
		std::vector<std::unique_ptr<VARIANT>>* values = new std::vector<std::unique_ptr<VARIANT>>();

		for (std::wstring property : *pProps) {
			VARIANT vtProp;
			HRESULT hr = pWmiObj->Get(
				bstr_t(property.c_str()),	// property being retrieved
				0,							// flags. Reserved: must be 0
				&vtProp,					// output pointer
				0,							// optional output: CIM type of property
				0);							// optionl output: origin of property

			values->push_back(std::make_unique<VARIANT>(vtProp));
		}

		return values;
	}


	///////////////////////////////////////
	//          Casting Objects          //
	///////////////////////////////////////

	bool WmiWrapper::wmiObjectToWstring(IWbemClassObject* pWmiObj, Json::Value *root) {
		if (pWmiObj == nullptr|| root == nullptr)
			return false;

		WmiWrapper::WmiWrapper::WmiMap* propMap = this->getWmiObjPropMap(pWmiObj);

		for (auto it = propMap->begin(); it != propMap->end(); it++) {
			std::string key = std::string(it->first.begin(), it->first.end());

			std::unique_ptr<VARIANT> value = std::move(it->second);
			auto valAsWbem                 = variantToWmiObj(value.get());

			if (valAsWbem != nullptr) {
				Json::Value node;
				wmiObjectToWstring(valAsWbem.get(), &node);
				(*root)[key] = node;
			}
			else {
				std::wstring val = this->variantToWstring(value.get());
				(*root)[key] = std::string(val.begin(), val.end());
			}

			VariantClear(value.get());
		} // end of for loop

		delete propMap;

		return true;
	}

	std::wstring WmiWrapper::wmiObjectToWstring(IWbemClassObject* pWmiObj) {
		Json::Value root;
		wmiObjectToWstring(pWmiObj, &root);

		Json::StreamWriterBuilder wbuilder;
		wbuilder["indentation"] = "\t";

		std::string outputString = Json::writeString(wbuilder, root);

		return std::wstring(outputString.begin(), outputString.end());
	}

	std::wstring WmiWrapper::variantToWstring(VARIANT *pVariant) {
		std::wostringstream ws;

		switch (V_VT(pVariant)) {
		case VT_BSTR:
			ws << V_BSTR(pVariant);
			break;
		case VT_I2:
			ws << V_I2(pVariant);
			break;
		case VT_ARRAY:
			ws << V_ARRAY(pVariant);
			break;
		case VT_UI1:
			ws << static_cast<unsigned int>(V_UI1(pVariant));
			break;
		case VT_ARRAY | VT_I4:
			variantArrayToString<int>(pVariant, &ws);
			break;
		case VT_ARRAY | VT_BSTR:
			variantArrayToString<BSTR>(pVariant, &ws);
			break;
		case VT_ARRAY | VT_UNKNOWN: {
			SAFEARRAY *psa = V_ARRAY(pVariant);
			IUnknown* pVals;
			HRESULT arrayAccesshr = SafeArrayAccessData(psa, (void **)&pVals);
			if (SUCCEEDED(arrayAccesshr))
			{
				long lLBound = -1, lUBound = 1;
				SafeArrayGetLBound(psa, 1, &lLBound);
				SafeArrayGetUBound(psa, 1, &lUBound);

				for (lLBound; lLBound <= lUBound; lLBound++) {
					IUnknown* pUnknown = static_cast<IUnknown*>(&(pVals[lLBound]));

					IWbemClassObject* obj = iUnknownToWmiObj(pUnknown);
					if (obj != nullptr)
						ws << wmiObjectToWstring(obj);
					else
						ws << "Likely array of MSFT objects. No known way to print.";
				} // end of safearray for loop

				SafeArrayUnaccessData(psa);
			}
			else
				ws << "Array of VARIANT type unknown.";
			break;
		}
		case VT_ARRAY | VT_UI1:
			variantArrayToString<char unsigned>(pVariant, &ws);
			break;

		case VT_BOOL: {
			bool value = V_BOOL(pVariant) == 0 ? false : true; // convert from V_BOOL where -1 = true
			ws << value;
			break;
		}
		case VT_I4:
			ws << V_I4(pVariant);
			break;
		case VT_R8:
			ws << V_R8(pVariant);
			break;
		case VT_UNKNOWN: {
			auto wmiObj = variantToWmiObj(pVariant);
			if (wmiObj != nullptr)
				wmiObjectToWstring(wmiObj.get());
			else
				ws << "VARIANT type unknown";

			break;
		}
		default:
			if (V_VT(pVariant) != VT_NULL && V_VT(pVariant) != VT_EMPTY) {
				ws << "VARIANT type unsuported: 0x" << std::hex << V_VT(pVariant) << std::dec;
			}

		} //end switch statement

		return ws.str();
	}

	template <class ArrayType>
	HRESULT WmiWrapper::variantArrayToString(VARIANT *pVariant, std::wostringstream *ws) {
		// Generate SAFEARRAY of ArrayType
		SAFEARRAY *psa = V_ARRAY(pVariant);
		ArrayType* pVals;
		HRESULT arrayAccesshr = SafeArrayAccessData(psa, (void **)&pVals);

		// Print SAFEARRAY
		if (SUCCEEDED(arrayAccesshr))
		{
			long lLBound = -1, lUBound = 1;
			SafeArrayGetLBound(psa, 1, &lLBound);
			SafeArrayGetUBound(psa, 1, &lUBound);

			for (lLBound; lLBound <= lUBound; lLBound++)
				*ws << pVals[lLBound] << " ";

			SafeArrayUnaccessData(psa);
		}

		return arrayAccesshr;
	}

	std::unique_ptr<IWbemClassObject, void(*)(IWbemClassObject*)> WmiWrapper::variantToWmiObj(VARIANT* pVariant) {
		try {
			_variant_t variant(pVariant);

			// Turn variant into IUnknown
			IUnknown* pUnknown = nullptr;
			pUnknown = static_cast<IUnknown*>(variant);

			std::unique_ptr< IWbemClassObject, void(*)(IWbemClassObject*)> wmiObj(iUnknownToWmiObj(pUnknown), [](IWbemClassObject* wmiObj) {
				wmiObj->Release();
			});

			// Cleanup
			pUnknown->Release();

			return std::move(wmiObj);
		}
		catch (...) {
			return std::unique_ptr< IWbemClassObject, void(*)(IWbemClassObject*)>(nullptr, nullptr);
		}
	}

	IWbemClassObject* WmiWrapper::WmiWrapper::iUnknownToWmiObj(IUnknown * pUnknown) {
		IWbemClassObject* pOutput = nullptr;

		// Catch Windows exceptions
		HRESULT hr;
		__try {
			hr = pUnknown->QueryInterface(IID_IWbemClassObject, reinterpret_cast<void**>(&pOutput));
		}
		__except (1) {
			return nullptr;
		}

		if (SUCCEEDED(hr))
			return pOutput;
		return nullptr;
	}

}