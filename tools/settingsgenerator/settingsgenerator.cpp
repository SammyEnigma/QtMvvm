#include "settingsgenerator.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>

#define TABS QString{QLatin1Char('\t')}.repeated(intendent)

SettingsGenerator::SettingsGenerator(const QString &hdrPath, const QString &srcPath) :
	_hdrFile{hdrPath},
	_srcFile{srcPath},
	_hdr{&_hdrFile},
	_src{&_srcFile}
{}

void SettingsGenerator::process(const QString &inPath)
{
	// read settings and adjust defaults
	auto settings = readDocument(inPath);
	if(!settings.name)
		settings.name = QFileInfo{_hdrFile.fileName()}.baseName();

	if(!_hdrFile.open(QIODevice::WriteOnly | QIODevice::Text))
		throw FileException{_hdrFile};
	writeHeader(settings);
	_hdrFile.close();

	if(!_srcFile.open(QIODevice::WriteOnly | QIODevice::Text))
		throw FileException{_srcFile};
	writeSource(settings);
	_srcFile.close();
}

bool SettingsGenerator::read_type_mapping(QXmlStreamReader &reader, QHash<QString, QString> &data, bool hasNext)
{
	TypeMappingGroup grp;
	hasNext = read_TypeMappingGroup(reader, grp, hasNext);
	for(const auto &mapping : qAsConst(grp.typeMapping))
		data.insert(mapping.key, mapping.type);
	return hasNext;
}

void SettingsGenerator::read_included_file(QXmlStreamReader &reader, NodeContentGroup &data)
{
	ImportType import;
	read_ImportType(reader, import);

	//make the path relative if possbile
	if(dynamic_cast<QFileDevice*>(reader.device())) {
		QFileInfo docInfo{static_cast<QFileDevice*>(reader.device())->fileName()};
		import.importPath = docInfo.dir().absoluteFilePath(import.importPath);
	}

	// read the document
	try {
		QFile xmlFile{import.importPath};
		if(!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text))
			throw FileException{xmlFile};

		QXmlStreamReader subReader{&xmlFile};
		if(!subReader.readNextStartElement())
			throw XmlException{subReader};

		SettingsType settings;
		if(subReader.name() == QStringLiteral("Settings")) {
			read_SettingsType(subReader, settings);
		} else if(subReader.name() == QStringLiteral("SettingsConfig")) {
			SettingsTranslator confReader;
			SettingsConfigBase::SettingsConfigType settingsConf;
			confReader.read_SettingsConfigType(subReader, settingsConf);
			convertFromConf(reader, settingsConf, settings);
		} else
			throwChild(subReader);

		NodeContentGroup *cGrp = &settings;
		if(import.rootNode) {
			for(const auto &key : import.rootNode.value().split(QLatin1Char('/'), QString::SkipEmptyParts)) {
				cGrp = findContentGroup(cGrp, key);
				if(!cGrp)
					return;
			}
		}
		data = std::move(*cGrp);
	} catch(FileException &e) {
		if(import.required)
			throw;
		else {
			qWarning() << e.what();
			return;
		}
	}
}

void SettingsGenerator::convertFromConf(QXmlStreamReader &reader, SettingsConfigBase::SettingsConfigType &conf, SettingsType &data)
{
	for(auto &element : conf.content) {
		if(nonstd::holds_alternative<SettingsConfigBase::CategoryType>(element))
			readCategory(reader, nonstd::get<SettingsConfigBase::CategoryType>(element), data);
		else if(nonstd::holds_alternative<SettingsConfigBase::SectionType>(element))
			readSection(reader, nonstd::get<SettingsConfigBase::SectionType>(element), data);
		else if(nonstd::holds_alternative<SettingsConfigBase::GroupType>(element))
			readGroup(reader, nonstd::get<SettingsConfigBase::GroupType>(element), data);
		else if(nonstd::holds_alternative<SettingsConfigBase::EntryType>(element))
			readEntry(reader, nonstd::get<SettingsConfigBase::EntryType>(element), data);
		else
			throw XmlException{reader, QStringLiteral("Unexpected child element in included SettingsConf")};
	}
}

void SettingsGenerator::readCategory(QXmlStreamReader &reader, SettingsConfigBase::CategoryContentGroup &content, NodeContentGroup &targetRootNode)
{
	for(auto &element : content.content) {
		if(nonstd::holds_alternative<SettingsConfigBase::SectionType>(element))
			readSection(reader, nonstd::get<SettingsConfigBase::SectionType>(element), targetRootNode);
		else if(nonstd::holds_alternative<SettingsConfigBase::GroupType>(element))
			readGroup(reader, nonstd::get<SettingsConfigBase::GroupType>(element), targetRootNode);
		else if(nonstd::holds_alternative<SettingsConfigBase::EntryType>(element))
			readEntry(reader, nonstd::get<SettingsConfigBase::EntryType>(element), targetRootNode);
		else
			throw XmlException{reader, QStringLiteral("Unexpected child element in included SettingsConf Categoy")};
	}
}

void SettingsGenerator::readSection(QXmlStreamReader &reader, SettingsConfigBase::SectionContentGroup &content, NodeContentGroup &targetRootNode)
{
	for(auto &element : content.content) {
		if(nonstd::holds_alternative<SettingsConfigBase::GroupType>(element))
			readGroup(reader, nonstd::get<SettingsConfigBase::GroupType>(element), targetRootNode);
		else if(nonstd::holds_alternative<SettingsConfigBase::EntryType>(element))
			readEntry(reader, nonstd::get<SettingsConfigBase::EntryType>(element), targetRootNode);
		else
			throw XmlException{reader, QStringLiteral("Unexpected child element in included SettingsConf Section")};
	}
}

void SettingsGenerator::readGroup(QXmlStreamReader &reader, SettingsConfigBase::GroupContentGroup &content, NodeContentGroup &targetRootNode)
{
	for(auto &element : content.content) {
		if(nonstd::holds_alternative<SettingsConfigBase::EntryType>(element))
			readEntry(reader, nonstd::get<SettingsConfigBase::EntryType>(element), targetRootNode);
		else
			throw XmlException{reader, QStringLiteral("Unexpected child element in included SettingsConf Group")};
	}
}

void SettingsGenerator::readEntry(QXmlStreamReader &reader, SettingsConfigBase::EntryType &entry, NodeContentGroup &targetRootNode)
{
	auto keyChain = entry.key.split(QLatin1Char('/'), QString::SkipEmptyParts);
	auto entryKey = keyChain.takeLast();

	auto cGrp = &targetRootNode;
	for(const auto &key : keyChain) {
		auto nGrp = findContentGroup(cGrp, key);
		if(!nGrp) {
			NodeType nNode;
			nNode.key = key;
			cGrp->contentNodes.append(std::move(nNode));
			nGrp = &(nonstd::get<NodeType>(cGrp->contentNodes.last()));
		}
		cGrp = nGrp;
	}

	auto isEntry = false;
	auto eGrp = findContentGroup(cGrp, entryKey, &isEntry);
	if(!eGrp) {
		EntryType nEntry;
		nEntry.key = entryKey;
		cGrp->contentNodes.append(std::move(nEntry));
		eGrp = &(nonstd::get<EntryType>(cGrp->contentNodes.last()));
	} else if(!isEntry) {
		EntryType nEntry;
		nEntry.key = entryKey;
		nEntry.contentNodes = std::move(eGrp->contentNodes);
		eGrp = replaceNodeByEntry(cGrp, eGrp, std::move(nEntry));
		Q_ASSERT(eGrp);
	} else
		throw XmlException{reader, QStringLiteral("Found duplicated entry with key: %1").arg(entry.key)};

	auto nEntry = static_cast<EntryType*>(eGrp);
	nEntry->type = std::move(entry.type);
	nEntry->defaultValue = std::move(entry.defaultValue);
	nEntry->tr = std::move(entry.trdefault);
}

SettingsGeneratorBase::NodeContentGroup *SettingsGenerator::findContentGroup(SettingsGeneratorBase::NodeContentGroup *cGrp, const QString &key, bool *isEntry)
{
	if(isEntry)
		*isEntry = false;
	for(auto &cNode : cGrp->contentNodes) {
		if(nonstd::holds_alternative<NodeType>(cNode)) {
			if(nonstd::get<NodeType>(cNode).key == key)
				return &(nonstd::get<NodeType>(cNode));
		} else if(nonstd::holds_alternative<EntryType>(cNode)) {
			if(nonstd::get<EntryType>(cNode).key == key) {
				if(isEntry)
					*isEntry = true;
				return &(nonstd::get<EntryType>(cNode));
			}
		} else if(nonstd::holds_alternative<NodeContentGroup>(cNode)) {
			auto res = findContentGroup(&(nonstd::get<NodeContentGroup>(cNode)), key);
			if(res)
				return res;
		}
	}

	return nullptr;
}

SettingsGeneratorBase::NodeContentGroup *SettingsGenerator::replaceNodeByEntry(SettingsGeneratorBase::NodeContentGroup *cGrp, NodeContentGroup *node, SettingsGeneratorBase::EntryType &&entry)
{
	for(auto &cNode : cGrp->contentNodes) {
		if(nonstd::holds_alternative<NodeType>(cNode)) {
			if(&nonstd::get<NodeType>(cNode) == node) {
				cNode = std::move(entry);
				return &(nonstd::get<EntryType>(cNode));
			}
		} else if(nonstd::holds_alternative<NodeContentGroup>(cNode)) {
			auto res = replaceNodeByEntry(&(nonstd::get<NodeContentGroup>(cNode)), node, std::move(entry));
			if(res)
				return res;
		}
	}

	return nullptr;
}

void SettingsGenerator::writeHeader(const SettingsType &settings)
{
	QString incGuard = QFileInfo{_hdrFile.fileName()}
					   .completeBaseName()
					   .replace(QLatin1Char('.'), QLatin1Char('_'))
					   .toUpper() + QStringLiteral("_H");
	_hdr << "#ifndef " << incGuard << '\n'
		 << "#define " << incGuard << "\n\n";

	// write the includes
	auto includes = QList<IncludeType> {
		{false, QStringLiteral("QtCore/QObject")},
		{false, QStringLiteral("QtMvvmCore/ISettingsAccessor")},
		{false, QStringLiteral("QtMvvmCore/SettingsEntry")}
	} + settings.includes;
	for(const auto &inc : includes) {
		if(inc.local)
			_hdr << "#include \"" << inc.includePath << "\"\n";
		else
			_hdr << "#include <" << inc.includePath << ">\n";
	}
	_hdr << "\n";

	// create the class
	_hdr << "class " << QString{settings.prefix ? settings.prefix.value() + QLatin1Char(' ') + settings.name.value() : settings.name.value()} << " : public QObject\n"
		 << "{\n"
		 << "\tQ_OBJECT\n\n"
		 << "\tQ_PROPERTY(QtMvvm::ISettingsAccessor *accessor READ accessor CONSTANT FINAL)\n\n"
		 << "public:\n"
		 << "\tQ_INVOKABLE explicit " << settings.name.value() << "(QObject *parent = nullptr);\n"
		 << "\texplicit " << settings.name.value() << "(QtMvvm::ISettingsAccessor *accessor, QObject *parent);\n\n"
		 << "\tstatic " << settings.name.value() << " *instance();\n\n"
		 << "\tQtMvvm::ISettingsAccessor *accessor() const;\n\n";

	writeNodeElements(settings, settings.typeMappings);

	_hdr << "\nprivate:\n"
		 << "\tQtMvvm::ISettingsAccessor *_accessor;\n"
		 << "};\n\n"
		 << "#endif //" << incGuard << '\n';
}

void SettingsGenerator::writeNodeElements(const NodeContentGroup &node, const QHash<QString, QString> &typeMappings, int intendent)
{
	for(const auto &cNode : node.contentNodes) {
		if(nonstd::holds_alternative<NodeType>(cNode))
			writeNode(nonstd::get<NodeType>(cNode), typeMappings, intendent);
		else if(nonstd::holds_alternative<EntryType>(cNode))
			writeEntry(nonstd::get<EntryType>(cNode), typeMappings, intendent);
		else if(nonstd::holds_alternative<NodeContentGroup>(cNode))
			writeNodeElements(nonstd::get<NodeContentGroup>(cNode), typeMappings, intendent);
	}
}

void SettingsGenerator::writeNode(const NodeType &node, const QHash<QString, QString> &typeMappings, int intendent)
{
	_hdr << TABS << "struct { //" << node.key << "\n";
	writeNodeElements(node, typeMappings, intendent + 1);
	_hdr << TABS << "} " << node.key << ";\n";
}

void SettingsGenerator::writeEntry(const EntryType &entry, const QHash<QString, QString> &typeMappings, int intendent)
{
	if(entry.contentNodes.isEmpty())
		_hdr << TABS << "QtMvvm::SettingsEntry<" << typeMappings.value(entry.type, entry.type) << "> " << entry.key << ";\n";
	else {
		_hdr << TABS << "struct : QtMvvm::SettingsEntry<" << typeMappings.value(entry.type, entry.type) << "> { //" << entry.key << "\n";
		writeNodeElements(entry, typeMappings, intendent + 1);
		_hdr << TABS << "} " << entry.key << ";\n";
	}
}

void SettingsGenerator::writeSource(const SettingsType &settings)
{
	_src << "#include \"" << _hdrFile.fileName() << "\"\n";
	if(!settings.backend)
		_src << "#include <QtMvvmCore/QSettingsAccessor>\n";
	_src << "\n";

	auto backend = settings.backend.value_or(BackendType{QStringLiteral("QtMvvm::QSettingsAccessor"), {}});
	_src << settings.name.value() << "::" << settings.name.value() << "(QObject *parent) : \n"
		 << "\t" << settings.name.value() << "{new " << backend.className << "{";
	if(!backend.param.isEmpty()) {
		_src << "\n";
		auto first = true;
		for(const auto &param : qAsConst(backend.param)) {
			if(first)
				first = false;
			else
				_src << ",\n";
			_src << "\t\tQVariant{";
			if(param.asStr)
				_src << "QStringLiteral(\"" << param.value << "\")";
			else
				_src << param.value;
			_src << "}.value<" << param.type << ">()";
		}
		_src << "\n\t";
	}
	_src << "}, parent}\n"
		 << "{\n"
		 << "\t_accessor->setParent(this);\n"
		 << "}\n\n";

	_src << settings.name.value() << "::" << settings.name.value() << "(QtMvvm::ISettingsAccessor *accessor, QObject *parent) : \n"
		 << "\tQObject{parent},\n"
		 << "\t_accessor{accessor}\n"
		 << "{}\n\n";

	_src << settings.name.value() << " *" << settings.name.value() << "::instance()\n"
		 << "{\n"
		 << "\treturn nullptr;\n"
		 << "}\n\n";

	_src << "QtMvvm::ISettingsAccessor *" << settings.name.value() << "::accessor() const\n"
		 << "{\n"
		 << "\treturn _accessor;\n"
		 << "}\n\n";
}
