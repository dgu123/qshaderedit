
#include "effect.h"
#include "messagepanel.h"
#include "outputparser.h"
#include "texmanager.h"
#include "parameter.h"
#include "glutils.h"

#include <QtCore/QDebug> //
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QByteArray>
#include <QtCore/QTime>
#include <QtCore/QVariant>
#include <QtCore/QHash>
#include <QtCore/QLibrary>
#include <QtCore/QThread>
#include <QtCore/QFileInfo>
#include <QtOpenGL/QGLContext>
#include <QtOpenGL/QGLPixelBuffer>

//#if HAVE_CG
#include <Cg/cg.h>
#include <Cg/cgGL.h>
#if CG_VERSION_NUM < 1400
#	error "Cg 1.4 required"
#endif
//#else
//#include "cgdyn.h"
//#endif

namespace {

	// Default vertex shader.
	static const char * s_effectText =
		"float3 color : DIFFUSE < string SasUiLabel = \"Base Color\"; > = {1.0f, 0.0f, 0.0f};\n"
		"float amb < string SasUiLabel = \"Ambient\"; float SasUiMin = 0; float SasUiMax = 1; > = 0.1f;\n\n"
		"float4x4 mvp : ModelViewProjection;\n"
		"float3x3 normalMatrix : ModelViewInverseTranspose;\n\n"
		"struct VertexInput {\n"
		"	float4 position : POSITION;\n"
		"	float3 normal : NORMAL;\n"
		"};\n\n"
		"struct VertexOutput {\n"
		"	float4 position : POSITION;\n"
		"	float3 normal : TEXCOORD0;\n"
		"};\n\n"
		"VertexOutput vp(VertexInput input)\n"
		"{\n"
		"	VertexOutput output;\n"
		"	output.position = mul(input.position, mvp);\n"
		"	output.normal = mul(input.normal, normalMatrix);\n"
		"	return output;\n"
		"}\n\n"
		"float4 fp(VertexOutput input) : COLOR\n"
		"{\n"
		"	float3 N = normalize(input.normal);\n"
		"	float3 L = normalize(gl_LightSource[0].position.xyz);\n"
		"	float3 H = normalize(gl_LightSource[0].halfVector.xyz);\n\n"
		"	float3 ambient = color * amb;\n"
		"	float3 diffuse = color * (1.0 - amb) * saturate(dot(L, N));\n"
		"	float3 specular = pow(saturate(dot(H, N)), 16.0);\n\n"
		"	return float4(ambient + diffuse + specular, 1.0);\n"
		"}\n\n"
		"technique main\n"
		"{\n"
		"	pass p0\n"
		"	{\n"
		"		VertexProgram = compile arbvp1 vp();\n"
		"		FragmentProgram = compile arbfp1 fp();\n"
		"		DepthTestEnable = true;\n"
		"		CullFaceEnable = true;\n"
		"	}\n"
		"}\n";

	struct CgSemantic
	{
		enum Type
		{
			Type_Null,
			Type_IdentityMatrix,
			Type_GLMatrix,
			Type_Time,
			Type_ViewportSize,
		};

		Type m_type;
		CGGLenum m_matrix;
		CGGLenum m_op;

	public:

		CgSemantic() : m_type(Type_Null)
		{
		}

		CgSemantic(Type type) : m_type(type)
		{
		}

		CgSemantic(CGGLenum matrix, CGGLenum op) : m_type(Type_GLMatrix), m_matrix(matrix), m_op(op)
		{
		}
	};

	static QHash<QString, CgSemantic> s_semanticMap;


	class CgParameter : public Parameter
	{
	private:
		QString m_internalName;
		CGparameter m_handle;
		bool m_visible;
		bool m_standard;

	public:
		CgParameter() {}
		
		CgParameter(CGparameter h, QVariant value) : m_handle(h), m_visible(true), m_standard(false)
		{
			Q_ASSERT(h != NULL);
			
			m_internalName = cgGetParameterName(m_handle);
			setName(m_internalName);
			
			if (value.isValid()) setValue(value);
			else setValue(getParameterValue(m_handle));
			
			readAnnotations();
			readSemantic();
			setVisibility();
		}

		CGparameter handle() const
		{
			return m_handle;
		}
		
		const QString & internalName() const
		{
			return m_internalName;
		}
		
		virtual int rows() const
		{
			int num = cgGetParameterColumns(m_handle);
			
			CGparameterclass parameterClass = cgGetParameterClass(m_handle);
			if( parameterClass == CG_PARAMETERCLASS_VECTOR ) {
				return num;
			}
			else if( parameterClass == CG_PARAMETERCLASS_MATRIX ) {
				return num;
			}
			
			return 0;
		}

		virtual int columns() const
		{
			int num = cgGetParameterRows(m_handle);
			
			CGparameterclass parameterClass = cgGetParameterClass(m_handle);
			if( parameterClass == CG_PARAMETERCLASS_VECTOR ) {
				return 1;
			}
			else if( parameterClass == CG_PARAMETERCLASS_MATRIX ) {
				return num;
			}
			
			return 0;
		}

		bool isHidden() const
		{
			return !m_visible;
		}

		bool isStandard() const
		{
			return m_standard;
		}

	private:

		void readAnnotations()
		{
			CGannotation annotation = cgGetFirstParameterAnnotation(m_handle);
			
			QVariant min, max;
			
			while(annotation != NULL) {
				
				const char * name = cgGetAnnotationName(annotation);
				
				// Override UI name.
				if( qstricmp(name, "UIHelp") == 0 || qstricmp(name, "UIName") == 0 ||
					qstricmp(name, "SasUiLabel") == 0) 
				{
					setName(cgGetStringAnnotationValue(annotation));
				}
				
				// Get description.
				if( qstricmp(name, "UIDescription") == 0 || qstricmp(name, "SasUiDescription") == 0) {
					setDescription(cgGetStringAnnotationValue(annotation));
				}
				
				// UI widget.
				else if( qstricmp(name, "UIWidget") == 0 || qstricmp(name, "SasUiControl") == 0) {
					const char * annotationValue = cgGetStringAnnotationValue(annotation);
					if(qstricmp("None", annotationValue) == 0) {
						m_visible = false;
					}
					else if(qstricmp("ColorPicker", annotationValue) == 0 || qstricmp("Color", annotationValue) == 0) {
						setWidget( Widget_Color);
					}
				}
				
				// Range
				else if( qstricmp(name, "UIMin") == 0 || qstricmp(name, "SasUiMin") == 0) {
					min = getAnnotationValue(annotation);
				}
				else if( qstricmp(name, "UIMax") == 0 || qstricmp(name, "SasUiMax") == 0) {
					max = getAnnotationValue(annotation);
				}
				
				else if( qstricmp(name, "SasUiVisible") == 0 ) {
					int num;
					const CGbool * values = cgGetBoolAnnotationValues(annotation, &num);
					Q_ASSERT(num > 0);
					m_visible = (values[0] != 0); 
				}
				
				// ResourceName
				else if( qstricmp(name, "ResourceName") ) {
					// This is already handled in getParameterValue.
				}
				
				annotation = cgGetNextAnnotation(annotation);
			}
			
			if( min.isValid() && max.isValid() ) {
				this->setRange(min, max);
			}
		}
		
		void readSemantic()
		{
			const char * semantic = cgGetParameterSemantic(m_handle);
			if(semantic == NULL) return;
			
			QString key = QString(semantic).toLower();
			
			if( s_semanticMap.contains(key) ) {
				m_standard = true;
			}
			else if( qstricmp("diffuse", semantic) == 0 || qstricmp("specular", semantic) == 0 ) {
				if( cgGetParameterClass(m_handle) == CG_PARAMETERCLASS_VECTOR ) {
					setWidget(Widget_Color);
				}
 			}
		}
		
		void setVisibility()
		{
			CGparameterclass parameterClass = cgGetParameterClass(m_handle);
			
			Q_ASSERT(parameterClass != CG_PARAMETERCLASS_STRUCT);
			Q_ASSERT(parameterClass != CG_PARAMETERCLASS_ARRAY);

			if( parameterClass == CG_PARAMETERCLASS_UNKNOWN ||
				parameterClass == CG_PARAMETERCLASS_OBJECT )
			{
				m_visible = false;
			}
			else {
				CGenum variability = cgGetParameterVariability(m_handle);
			
				if( variability != CG_UNIFORM ) {
					// Hide const, literal and varying parameters.
					m_visible = false;
				}
			}
		}

		static QVariant getParameterValue(CGparameter parameter)
		{
			CGtype parameterBaseType = cgGetParameterBaseType(parameter);
			CGparameterclass parameterClass = cgGetParameterClass(parameter);

			Q_ASSERT(parameterClass != CG_PARAMETERCLASS_STRUCT);
			Q_ASSERT(parameterClass != CG_PARAMETERCLASS_ARRAY);

			if(parameterClass == CG_PARAMETERCLASS_OBJECT) {
				if(parameterBaseType == CG_STRING) {
					return cgGetStringParameterValue(parameter);
				}
				else if(parameterBaseType == CG_TEXTURE) {
					// Ignore textures.
				}
			}
			else if(parameterClass == CG_PARAMETERCLASS_SAMPLER) {
				// Get ResourceName annotation.
				CGannotation annotation = cgGetNamedParameterAnnotation(parameter, "ResourceName");
				if(annotation == 0) {
					// @@ Hack! state assignment name must be lowercase!
					CGstateassignment sa = cgGetNamedSamplerStateAssignment(parameter, "texture");
					if(sa != 0) {
						CGparameter p = cgGetTextureStateAssignmentValue(sa);
						if(p != 0) {
							annotation = cgGetNamedParameterAnnotation(p, "ResourceName");
						}
					}
				}

				// Get resource string.
				if(annotation == 0) {
					return qVariantFromValue(GLTexture());
				}
				else {
					return qVariantFromValue(GLTexture::open(cgGetStringAnnotationValue(annotation)));
				}
			}
			else if(parameterClass == CG_PARAMETERCLASS_SCALAR) {
				int num;
				const double * values = cgGetParameterValues(parameter, CG_DEFAULT, &num);
				Q_ASSERT(num == 1);

				if(parameterBaseType == CG_FLOAT || parameterBaseType == CG_HALF || parameterBaseType == CG_FIXED) {
					return values[0];
				}
				else if(parameterBaseType == CG_INT) {
					return int(values[0]);
				}
				else if(parameterBaseType == CG_BOOL) {
					return bool(values[0]);
				}
			}
			else if(parameterClass == CG_PARAMETERCLASS_VECTOR || parameterClass == CG_PARAMETERCLASS_MATRIX) {
				int num;
				const double * values = cgGetParameterValues(parameter, CG_DEFAULT, &num);
				Q_ASSERT(num > 0);

				QVariantList list;
				if(parameterBaseType == CG_FLOAT || parameterBaseType == CG_HALF || parameterBaseType == CG_FIXED) {
					for(int i = 0; i < num; i++) {
						list.append(float(values[i]));
					}
				}
				else if(parameterBaseType == CG_INT) {
					for(int i = 0; i < num; i++) {
						list.append(int(values[i]));
					}
				}
				else if(parameterBaseType == CG_BOOL) {
					for(int i = 0; i < num; i++) {
						list.append(bool(values[i]));
					}
				}
				return list;
			}
			return QVariant();
		}

		static QVariant getAnnotationValue(CGannotation annotation)
		{
			CGtype annotationType = cgGetAnnotationType(annotation);
			
			int num;
			
			// For now we only support scalars.
			if(annotationType == CG_FLOAT || annotationType == CG_HALF || annotationType == CG_FIXED) {
				const float * values = cgGetFloatAnnotationValues(annotation, &num);
				Q_ASSERT(num == 1);
				return values[0];
			}
			else if(annotationType == CG_INT) {
				const int * values = cgGetIntAnnotationValues(annotation, &num);
				Q_ASSERT(num == 1);
				return values[0];
			}
			else if(annotationType == CG_BOOL) {
				const CGbool * values = cgGetBoolAnnotationValues(annotation, &num);
				Q_ASSERT(num == 1);
				return values[0];
			}
			
			return QVariant();
		}
	};

} // namespace



class CgFxEffect : public Effect
{
private:

	CGcontext m_context;
	CGeffect m_effect;
	CGtechnique m_technique;
	CGpass m_pass;

	QByteArray m_effectText;

	bool m_animated;
	QTime m_time;

	QVector<CgParameter *> m_parameterArray;

	QList<CGtechnique> m_techniqueList;
	QList<CGpass> m_passList;

	OutputParser* m_outputParser;

	QString m_effectPath;
	
	// Builder thread.
	class BuilderThread : public GLThread
	{
		CgFxEffect * m_effect;
	public:
		BuilderThread(CgFxEffect * effect, QGLWidget * widget) : GLThread(widget), m_effect(effect)
		{
		}
		void run() 
		{
			makeCurrent();
			m_effect->threadedBuild();
		}
	};
	friend class BuilderThread;
	BuilderThread m_thread;	

public:

	CgFxEffect(const EffectFactory * factory, QGLWidget * widget) : Effect(factory),
		m_context(NULL),
		m_effect(NULL),
		m_technique(NULL),
		m_pass(NULL),
		m_effectText(s_effectText),
		m_animated(false),
		m_thread(this, widget)
	{
		connect(&m_thread, SIGNAL(finished()), this, SIGNAL(built()));
		widget->makeCurrent();

		m_context = cgCreateContext();

		cgGLSetManageTextureParameters(m_context, true);
		cgGLRegisterStates(m_context);

		if( s_semanticMap.empty() ) {
			// Cg is broken and transposes all the matrices.
			s_semanticMap.insert("worldviewprojection", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_TRANSPOSE));
			s_semanticMap.insert("worldviewprojectiontranspose", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_IDENTITY));
			s_semanticMap.insert("worldviewprojectioninverse", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_INVERSE_TRANSPOSE));
			s_semanticMap.insert("worldviewprojectioninversetranspose", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_INVERSE));

			s_semanticMap.insert("modelviewprojection", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_TRANSPOSE));
			s_semanticMap.insert("modelviewprojectiontranspose", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_IDENTITY));
			s_semanticMap.insert("modelviewprojectioninverse", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_INVERSE_TRANSPOSE));
			s_semanticMap.insert("modelviewprojectioninversetranspose", CgSemantic(CG_GL_MODELVIEW_PROJECTION_MATRIX, CG_GL_MATRIX_INVERSE));

			s_semanticMap.insert("modelview", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_TRANSPOSE));
			s_semanticMap.insert("modelviewtranspose", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_IDENTITY));
			s_semanticMap.insert("modelviewinverse", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_INVERSE_TRANSPOSE));
			s_semanticMap.insert("modelviewinversetranspose", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_INVERSE));

			s_semanticMap.insert("view", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_TRANSPOSE));
			s_semanticMap.insert("viewtranspose", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_IDENTITY));
			s_semanticMap.insert("viewinverse", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_INVERSE_TRANSPOSE));
			s_semanticMap.insert("viewinversetranspose", CgSemantic(CG_GL_MODELVIEW_MATRIX, CG_GL_MATRIX_INVERSE));

			s_semanticMap.insert("world", CgSemantic(CgSemantic::Type_IdentityMatrix));
			s_semanticMap.insert("worldtranspose", CgSemantic(CgSemantic::Type_IdentityMatrix));
			s_semanticMap.insert("worldinverse", CgSemantic(CgSemantic::Type_IdentityMatrix));
			s_semanticMap.insert("worldinversetranspose", CgSemantic(CgSemantic::Type_IdentityMatrix));

			s_semanticMap.insert("projection", CgSemantic(CG_GL_PROJECTION_MATRIX, CG_GL_MATRIX_TRANSPOSE));
			s_semanticMap.insert("projectiontranspose", CgSemantic(CG_GL_PROJECTION_MATRIX, CG_GL_MATRIX_IDENTITY));

			s_semanticMap.insert("time", CgSemantic(CgSemantic::Type_Time));
			s_semanticMap.insert("viewportsize", CgSemantic(CgSemantic::Type_ViewportSize));
		}

		m_outputParser = new NvidiaOutputParser;

		m_time.start();
	}

	virtual ~CgFxEffect()
	{
		cgDestroyContext(m_context);
		delete m_outputParser;
		qDeleteAll(m_parameterArray);
	}


	// Load/Save the effect.
	virtual void load(QFile * file)
	{
		Q_ASSERT(file != NULL);

		m_effectText = file->readAll();
		
		QFileInfo info(*file);
		m_effectPath = info.path();

		m_time.start();
	}

	virtual void save(QFile * file)
	{
		Q_ASSERT(file != NULL);

		file->write(m_effectText);
	}

	virtual int getInputNum()
	{
		return 1;
	}

	virtual QString getInputName(int i)
	{
		Q_ASSERT(i == 0);
		return tr("Effect");
	}

	virtual const QByteArray & getInput(int i) const
	{
		Q_ASSERT(i == 0);
		return m_effectText;
	}

	virtual void setInput(int i, const QByteArray & str)
	{
		Q_ASSERT(i == 0);
		m_effectText = str;
	}

	
	void threadedBuild()
	{
		freeEffect();

		emit infoMessage(tr("Compiling cg effect..."));
		
		QString includeOption = "-I" + m_effectPath;
		const char * options[] = { includeOption.toAscii(), NULL };
		
		m_effect = cgCreateEffect(m_context, m_effectText.data(), options);
		
		// Output compilation errors.
		emit buildMessage(cgGetLastListing(m_context), 0, m_outputParser);

		if (m_effect == NULL)
		{
			freeEffect();
			return;
		}
		
		// Read and validate techniques.
		CGtechnique technique = cgGetFirstTechnique(m_effect);
		while(technique != NULL)
		{
			if (cgValidateTechnique(technique))
			{
				const char * name = cgGetTechniqueName(technique);
				emit infoMessage(tr("Validated technique '%1'").arg(name));
				m_techniqueList.append(technique);
			}

			// Output validation errors.
			emit buildMessage(cgGetLastListing(m_context), 0, m_outputParser);

			technique = cgGetNextTechnique(technique);
		}
		
		if (m_techniqueList.count() == 0)
		{
			freeEffect();
			return;
		}
		selectTechnique(0);

	//	error = cgGetError();
	//	if( error != CG_NO_ERROR ) {
	//		printf("%s\n", cgGetErrorString(error));
	//	}

		initParameters();
	}
	
	// Compilation.
	virtual void build(bool threaded)
	{
#if defined(Q_OS_LINUX)
		threaded = false;
#endif
		
		if (threaded) {
			m_thread.start();
		}
		else {
			threadedBuild();
			emit built();
		}
	}
	
	virtual bool isBuilding() const
	{
		return m_thread.isRunning();
	}

	// Parameter info.
	virtual int parameterCount() const
	{
		return m_parameterArray.count();
	}
	
	virtual const Parameter * parameterAt(int idx) const
	{
		Q_ASSERT(idx >= 0 && idx < parameterCount());
		return m_parameterArray.at(idx); 	
	}

	virtual Parameter * parameterAt(int idx)
	{
		Q_ASSERT(idx >= 0 && idx < parameterCount());
		return m_parameterArray.at(idx); 	
	}

	// Effect info.
	virtual bool isValid() const
	{
		return m_effect != 0 && m_technique != 0;
	}

	virtual bool isAnimated() const
	{
		return m_animated;
	}

	// Technique info.
	virtual int getTechniqueNum() const
	{
		return m_techniqueList.count();
	}
	virtual QString getTechniqueName(int t) const
	{
		Q_ASSERT(t <  getTechniqueNum());
		return cgGetTechniqueName(m_techniqueList.at(t));
	}
	virtual void selectTechnique(int t)
	{
		Q_ASSERT(t <  getTechniqueNum());
		m_technique = m_techniqueList.at(t);

		m_passList.clear();

		CGpass pass = cgGetFirstPass(m_technique);
		while(pass != NULL)
		{
			m_passList.append(pass);
			pass = cgGetNextPass(pass);
		}
	}

	// Pass info.
	virtual int getPassNum() const
	{
		Q_ASSERT(m_technique != NULL);
		return m_passList.count();
	}

	// Rendering.
	virtual void begin()
	{
		Q_ASSERT(m_technique != NULL);
		Q_ASSERT(m_pass == NULL);

		// Set standard parameter values.
		CGparameter parameter = cgGetFirstLeafEffectParameter(m_effect);
		while(parameter != NULL) {
			if(cgIsParameterUsed(parameter, m_effect)) {
				QString semantic = cgGetParameterSemantic(parameter);
				semantic = semantic.toLower();

				if( semantic != NULL && s_semanticMap.contains(semantic) ) {
					const CgSemantic & std = s_semanticMap[semantic];

					if( std.m_type == CgSemantic::Type_GLMatrix ) {
						cgGLSetStateMatrixParameter(parameter, std.m_matrix, std.m_op);
					}
					else if( std.m_type == CgSemantic::Type_IdentityMatrix ) {
						// @@ ???
					}
					else if( std.m_type == CgSemantic::Type_Time ) {
						cgSetParameter1f(parameter, 0.001f * m_time.elapsed());
					}
					else if( std.m_type == CgSemantic::Type_ViewportSize ) {
						GLfloat v[4];
						glGetFloatv(GL_VIEWPORT, v);
						cgSetParameter2f(parameter, v[2], v[3]);
					}
				}
			}
			parameter = cgGetNextLeafParameter(parameter);
		}

		// Set user parameters.
		foreach(const CgParameter * p, m_parameterArray) {
			parameter = p->handle();
			QVariant value = p->value();

			// Set cg parameter.
			CGparameterclass parameterClass = cgGetParameterClass(parameter);
		//	CGtype parameterType = cgGetParameterType(parameter);
		//	CGtype parameterBaseType = cgGetParameterBaseType(parameter);

			if( parameterClass == CG_PARAMETERCLASS_SCALAR ) {
				if( value.type() == QVariant::Double ) {
					cgSetParameter1d(parameter, value.toDouble());
				}
				else if( value.type() == QVariant::Int ) {
					cgSetParameter1i(parameter, value.toInt());
				}
				else if( value.type() == QVariant::Bool ) {
					cgSetParameter1i(parameter, value.toBool());
				}
			}
			else if( parameterClass == CG_PARAMETERCLASS_VECTOR ) {
				if (value.type() == QVariant::List) {
					QVariantList list = value.toList();
					
					int size = cgGetParameterColumns(parameter);	// Cg assumes row vectors!
					Q_ASSERT(list.count() == size);
					
					switch(size) {
						case 1:
							cgSetParameter1d(parameter, list.at(0).toDouble());
							break;
						case 2:
							cgSetParameter2d(parameter, list.at(0).toDouble(), list.at(1).toDouble());
							break;
						case 3:
							cgSetParameter3d(parameter, list.at(0).toDouble(), list.at(1).toDouble(), list.at(2).toDouble());
							break;
						case 4:
							cgSetParameter4d(parameter, list.at(0).toDouble(), list.at(1).toDouble(), list.at(2).toDouble(), list.at(3).toDouble());
							break;
					}
				}
			}
			else if( parameterClass == CG_PARAMETERCLASS_MATRIX ) {
				// @@ TBD
			}
			else if( parameterClass == CG_PARAMETERCLASS_SAMPLER ) {
				//cgGLSetTextureParameter(parameter, value.value<GLTexture>().object());
				cgGLSetupSampler(parameter, value.value<GLTexture>().object());
			}
			else if( parameterClass == CG_PARAMETERCLASS_OBJECT ) {
				// Ignore textures and strings.
			}
		}
	}
	virtual void beginPass(int p)
	{
		Q_ASSERT(p <  getPassNum());
		m_pass = m_passList.at(p);
		cgSetPassState(m_pass);
	}
	virtual void endPass()
	{
		cgResetPassState(m_pass);
		m_pass = NULL;
	}
	virtual void end()
	{
		Q_ASSERT(m_pass == NULL);
	}

private:

	void initParameters()
	{
		m_animated = false;
		QVector<CgParameter *> newParameterArray;
		
		// Read parameters.
		CGparameter parameter = cgGetFirstLeafEffectParameter(m_effect);
		while(parameter != NULL)
		{
			if(cgIsParameterUsed(parameter, m_effect))
			{
				// Try to get the old value.
				QVariant value;
				foreach(const CgParameter * p, m_parameterArray) {
					if( p->internalName() == cgGetParameterName(parameter) ) {
						value = p->value();
					}
				}
				
				CgParameter * cgParameter = new CgParameter(parameter, value);
				
				// Add non hidden and non standard parameters.
				if(!cgParameter->isHidden() && !cgParameter->isStandard()) {
					newParameterArray.append(cgParameter);
				}
				
				// Process semantics of standard parameters.
				if( cgParameter->isStandard() ) {
					QString semantic = cgGetParameterSemantic(parameter);
					semantic = semantic.toLower();
					
					if( s_semanticMap.contains(semantic) ) {
						const CgSemantic & std = s_semanticMap[semantic];
						if( std.m_type == CgSemantic::Type_Time ) {
							m_animated = true;
						}
					}
				}
			}
			parameter = cgGetNextLeafParameter(parameter);
		}

		qDeleteAll(m_parameterArray);
		m_parameterArray = newParameterArray;
	}
	
	void freeEffect()
	{
		if(m_effect != NULL) {
			cgDestroyEffect(m_effect);
			m_effect = NULL;
		}
		
		m_technique = NULL;
		m_pass = NULL;
		
		// resetParameters();
		
		m_techniqueList.clear();
		m_passList.clear();
	}

};



class CgFxEffectFactory : public EffectFactory
{
	virtual bool isSupported() const
	{
		// At least OpenGL 1.1 required.
		if (!GLEW_VERSION_1_1) {
			return false;
		}
		
		// @@ Do this only once!!
#if defined(Q_OS_DARWIN)
		QLibrary cgLibrary("/System/Library/Frameworks/Cg.framework/Cg");
		QLibrary cgGlLibrary("/System/Library/Frameworks/Cg.framework/CgGL");
#else
		QLibrary cgLibrary("Cg");
		QLibrary cgGlLibrary("CgGL");
#endif
		
		if (!cgLibrary.load())
		{
			qDebug("Cg library not found.");
			return false;
		}
		if (!cgGlLibrary.load())
		{
			qDebug("CgGL library not found.");
			return false;
		}
		
		//loadCgFunctions(cgLibrary, cgGlLibrary);

		typedef const char * (* GetString)(CGenum sname);			
		GetString cgGetString = (GetString) cgLibrary.resolve("cgGetString");

		if (cgGetString != NULL)
		{
			QRegExp rx("^(\\d)\\.(\\d).*");
			QString version = cgGetString(CG_VERSION);

			if(rx.exactMatch(version))
			{
				int major = rx.cap(1).toInt();
				int minor = rx.cap(2).toInt();

				if((major == 1 && minor >= 4) || major > 1)
				{
					return true;
				}
			}
			else
			{
				qDebug("cg version string not recognized.");
			}
		}
		
		return false;
	}

	virtual QString name() const
	{
		return tr("CgFx Effect");
	}

	virtual QString namePlural() const
	{
		return tr("CgFx Effects");
	}

	virtual QString extension() const
	{
		return "cgfx";
	}

	virtual QIcon icon() const
	{
		return QIcon();
	}

	virtual Effect * createEffect(QGLWidget * widget) const
	{
		Q_ASSERT(isSupported());
		return new CgFxEffect(this, widget);
	}

	virtual QList<Highlighter::Rule> highlightingRules() const
	{
		QList<Highlighter::Rule> rules;
		Highlighter::Rule rule;

		rule.type = Highlighter::Keyword;
		rule.pattern = QRegExp("\\b(if|else|for|while|do|struct|break|continue|discard|return|technique|pass|sampler_state|"
			"compile|true|false|packed|typedef)\\b");
		rules.append(rule);

		rule.type = Highlighter::DataType;
		rule.pattern = QRegExp(
			"\\b(void|float|float[1-4]|float[1-4]x[1-4]|int|int[1-4]|int[1-4]x[1-4]|bool|bool[1-4]|bool[1-4]x[1-4]|"
			"half|half[1-4]|half[1-4]x[1-4]|fixed|fixed[1-4]|fixed[1-4]x[1-4]|"
			"sampler[1-3]D|samplerCUBE|samplerRECT|texture|string|uniform|varying|static|const|in|out|inout)\\b");
		rules.append(rule);

		rule.type = Highlighter::BuiltinVar;
		rule.pattern = QRegExp(
			"\\b(gl_(Position|PointSize|ClipVertex|FragCoord|FragFacing|FragColor|"
			"FragData|FragDepth|Color|SecondaryColor|Normal|Vertex|MultiTexCoord[0-7]|FogColor|"
			"MaxLights|MaxClipPlanes|MaxTextureUnits|MaxTextureCoords|MaxVertexAttributes|"
			"MaxVertexUniformComponents|MaxVaryingFloats|MaxVertexTextureImageUnits|MaxCombinedTextureImageUnits|"
			"MaxTextureImageUnits|MaxFragmentUniformComponents|MaxDrawBuffers|ModelViewMatrix|ProjectionMatrix|"
			"ModelViewProjectionMatrix|TextureMatrix|NormalMatrix|ModelViewMatrixInverse|ProjectionMatrixInverse|"
			"ModelViewProjectionMatrixInverse|TextureMatrixInverse|ModelViewMatrixTranspose|ProjectionMatrixTranspose|"
			"ModelViewProjectionMatrixTranspose|TextureMatrixTranspose|ModelViewMatrixInverseTranspose|ProjectionMatrixInverseTranspose|"
			"ModelViewProjectionMatrixInverseTranspose|TextureMatrixInverseTranspose|NormScale|DepthRangeParameters|DepthRange|"
			"ClipPlane|PointParameters|Point|MaterialParameters|FrontMaterial|BackMaterial|LightSourceParameters|LightSource|"
			"LightModelParameters|LightModel|LightModelProducts|FrontLightModelProduct|BackLightModelProduct|LightProducts|"
			"FrontLightProduct|BackLightProduct|TextureEnvColor|EyePlaneS|EyePlaneT|EyePlaneR|EyePlaneQ|ObjectPlaneS|ObjectPlaneT|"
			"ObjectPlaneR|ObjectPlaneQ|FogParameters|Fog|FrontColor|BackColor|FrontSecondaryColor|BackSecondaryColor|TexCoord|FogFragCoord|Color|"
			"SecondaryColor)|"
			"WorldViewProjection(Inverse)?(Transpose)?|ModelView(Projection)?(Inverse)?(Transpose)?|View(Inverse)?(Transpose)?|"
			"World(Inverse)?(Transpose)?|Projection(Inverse)?(Transpose)?|Time|ViewportSize|"
			"MinFilter|MagFilter|WrapS|WrapT|BorderColor|"
			"POSITION|COLOR[0-1]?|TEXCOORD[0-7]?|NORMAL|"
			"VertexProgram|FragmentProgram|DepthTestEnable|CullFaceEnable|register\\(c[1-2]+\\))\\b");
		rules.append(rule);

		rule.type = Highlighter::BuiltinFunction;
		rule.pattern = QRegExp(
			"\\b(abs|acos|all|any|asin|atan|atan2|ceil|clamp|cos|cosh|cross|degrees|determinant|dot|floor|length|lerp|"
			"max|min|mul|normalize|radians|reflect|refract|round|saturate|sin|sinh|tan|tanh|transpose|"
			"tex1D(proj)?|tex2D(proj)?|texRECT(proj)?|tex3D(proj)?|texCUBE(proj)?|"
			"offsettex2D|offsettexRECT|offsettex2DScaleBias|offsettexRECTScaleBias|tex1D_dp3|tex2D_dp3x2|"
			"texRECT_dp3x2|tex3D_dp3x3|texCUBE_dp3x3|texCUBE_reflect_dp3x3|texCUBE_reflect_eye_dp3x3|tex_dp3x2_depth|"
			"(un)?pack_4(u)?byte|(un)?pack_2ushort|(un)?pack_2half)\\b");
		rules.append(rule);
		
		
		rule.type = Highlighter::Number;
		rule.pattern = QRegExp("\\b[-+]?\\d*\\.?\\d+([eE][-+]?\\d+)?[fFhHxX]?\\b");
		rules.append(rule);

		rule.type = Highlighter::String;
		rule.pattern = QRegExp("\".*\"");
		rules.append(rule);

		rule.type = Highlighter::Misc;
		rule.pattern = QRegExp("#.*(//|$)");
		rules.append(rule);

		rule.type = Highlighter::Comment;
		rule.pattern = QRegExp("//.*$");
		rules.append(rule);

		return rules;
	}

	QString multiLineCommentStart() const
	{
		return "/*";
	}

	QString multiLineCommentEnd() const
	{
		return "*/";
	}
};

REGISTER_EFFECT_FACTORY(CgFxEffectFactory);

