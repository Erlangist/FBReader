/*
 * Copyright (C) 2009-2010 Geometer Plus <contact@geometerplus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "OPDSLinkReader.h"
#include "OPDSLink.h"
#include "OPDSBasicAuthenticationManager.h"

OPDSLinkReader::OPDSLinkReader() : myState(READ_NOTHING) {
}

shared_ptr<NetworkLink> OPDSLinkReader::link() {
	if (mySiteName.empty() || myTitle.empty() || myLinks["main"].empty()) {
		return 0;
	}
	OPDSLink *opdsLink = new OPDSLink(
		mySiteName,
		myLinks["main"],
		myLinks["search"],
		myTitle,
		mySummary,
		myIconName
	);
	if (!mySearchType.empty()) {
		opdsLink->setupAdvancedSearch(
			mySearchType,
			mySearchParts["titleOrSeries"],
			mySearchParts["author"],
			mySearchParts["tag"],
			mySearchParts["annotation"]
		);
	}
	opdsLink->setIgnoredFeeds(myIgnoredFeeds);
	opdsLink->setAccountDependentFeeds(myAccountDependentFeeds);
	if (!myAuthenticationType.empty()) {
		shared_ptr<NetworkAuthenticationManager> mgr;
		if (myAuthenticationType == "basic") {
			mgr = new OPDSBasicAuthenticationManager(
				mySiteName,
				myAuthenticationParts["signInUrl"],
				myAuthenticationParts["signOutUrl"]
			);
		} /*else if (myAuthenticationType == "post") {
			mgr = new OPDSAuthenticationManager(
				mySiteName,
				myAuthenticationParts["login"],
				myAuthenticationParts["password"],
				myAuthenticationParts["signInUrl"],
				myAuthenticationParts["signOutUrl"],
				myAuthenticationParts["accountUrl"]
			);
		}*/
		if (!mgr.isNull()) {
			opdsLink->setAuthenticationManager(mgr);
		}
	}
	for (std::map<std::string,std::string>::const_iterator it = myUrlRewritingRules.begin(); it != myUrlRewritingRules.end(); ++it) {
		opdsLink->addUrlRewritingRule(it->first, it->second);
	}
	return opdsLink;
}

static const std::string TAG_SITE = "site";
static const std::string TAG_LINK = "link";
static const std::string TAG_TITLE = "title";
static const std::string TAG_SUMMARY = "summary";
static const std::string TAG_ICON = "icon";
static const std::string TAG_SEARCH_DESCRIPTION = "advancedSearch";
static const std::string TAG_PART = "part";
static const std::string TAG_IGNORED_FEEDS = "ignored-feeds";
static const std::string TAG_ACCOUNT_DEPENDENT_FEEDS = "account-dependent-feeds";
static const std::string TAG_AUTHENTICATION = "authentication";
static const std::string TAG_URL_REWRITING_RULES = "urlRewritingRules";
static const std::string TAG_ADD_URL_PARAMETER = "addUrlParameter";

void OPDSLinkReader::startElementHandler(const char *tag, const char **attributes) {
	if (TAG_SITE == tag) {
		myState = READ_SITENAME;
	} else if (myState == READ_NOTHING && TAG_LINK == tag) {
		const char *linkType = attributeValue(attributes, "type");
		if (linkType != 0) {
			myLinkType = linkType;
			myState = READ_LINK;
		}
	} else if (myState == READ_IGNORED && TAG_LINK == tag) {
		myLinkBuffer.clear();
		myState = READ_IGNORED_LINK;
	} else if (myState == READ_ACCOUNT_DEPENDENT && TAG_LINK == tag) {
		myLinkBuffer.clear();
		myState = READ_ACCOUNT_DEPENDENT_LINK;
	} else if (TAG_TITLE == tag) {
		myState = READ_TITLE;
	} else if (TAG_SUMMARY == tag) {
		myState = READ_SUMMARY;
	} else if (TAG_ICON == tag) {
		myState = READ_ICON_NAME;
	} else if (TAG_SEARCH_DESCRIPTION == tag) {
		const char *searchType = attributeValue(attributes, "style");
		if (searchType != 0) {
			mySearchType = searchType;
			myState = READ_SEARCH_DESCRIPTION;
		}
	} else if (myState == READ_SEARCH_DESCRIPTION && TAG_PART == tag) {
		const char *name = attributeValue(attributes, "name");
		if (name != 0) {
			mySearchPartName = name;
			myState = READ_SEARCH_PART;
		}
	} else if (TAG_IGNORED_FEEDS == tag) {
		myState = READ_IGNORED;
	} else if (TAG_ACCOUNT_DEPENDENT_FEEDS == tag) {
		myState = READ_ACCOUNT_DEPENDENT;
	} else if (TAG_AUTHENTICATION == tag) {
		const char *authenticationType = attributeValue(attributes, "type");
		if (authenticationType != 0) {
			myAuthenticationType = authenticationType;
			myState = READ_AUTHENTICATION_DESCRIPTION;
		}
	} else if (myState == READ_AUTHENTICATION_DESCRIPTION && TAG_PART == tag) {
		const char *name = attributeValue(attributes, "name");
		if (name != 0) {
			myAuthenticationPartName = name;
			myState = READ_AUTHENTICATION_PART;
		}
	} else if (TAG_URL_REWRITING_RULES == tag) {
		myState = READ_URL_REWRITING_RULES;
	} else if (myState == READ_URL_REWRITING_RULES && TAG_ADD_URL_PARAMETER == tag) {
		const char *name = attributeValue(attributes, "name");
		const char *value = attributeValue(attributes, "value");
		if (name != 0 && value != 0) {
			myUrlRewritingRules[name] = value;
		}
	}
}

void OPDSLinkReader::endElementHandler(const char*) {
	if (myState == READ_SEARCH_PART) {
		myState = READ_SEARCH_DESCRIPTION;
	} else if (myState == READ_AUTHENTICATION_PART) {
		myState = READ_AUTHENTICATION_DESCRIPTION;
	} else if (myState == READ_IGNORED_LINK) {
		myIgnoredFeeds.insert(myLinkBuffer);
		myState = READ_IGNORED;
	} else if (myState == READ_ACCOUNT_DEPENDENT_LINK) {
		myAccountDependentFeeds.insert(myLinkBuffer);
		myState = READ_ACCOUNT_DEPENDENT;
	} else {
		myState = READ_NOTHING;
	}
}

void OPDSLinkReader::characterDataHandler(const char *text, size_t len) {
	switch (myState) {
		case READ_NOTHING:
		case READ_SEARCH_DESCRIPTION:
		case READ_IGNORED:
		case READ_ACCOUNT_DEPENDENT:
		case READ_AUTHENTICATION_DESCRIPTION:
		case READ_URL_REWRITING_RULES:
			break;
		case READ_SITENAME:
			mySiteName.append(text, len);
			break;
		case READ_TITLE:
			myTitle.append(text, len);
			break;
		case READ_SUMMARY:
			mySummary.append(text, len);
			break;
		case READ_LINK:
			myLinks[myLinkType].append(text, len);
			break;
		case READ_ICON_NAME:
			myIconName.append(text, len);
			break;
		case READ_SEARCH_PART:
			mySearchParts[mySearchPartName].append(text, len);
			break;
		case READ_IGNORED_LINK:
		case READ_ACCOUNT_DEPENDENT_LINK:
			myLinkBuffer.append(text, len);
			break;
		case READ_AUTHENTICATION_PART:
			myAuthenticationParts[myAuthenticationPartName].append(text, len);
			break;
	}
}
